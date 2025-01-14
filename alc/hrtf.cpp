/**
 * OpenAL cross platform audio library
 * Copyright (C) 2011 by Chris Robinson
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include "hrtf.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <type_traits>
#include <utility>

#include "AL/al.h"

#include "alcmain.h"
#include "alconfig.h"
#include "alfstream.h"
#include "almalloc.h"
#include "alnumeric.h"
#include "aloptional.h"
#include "alspan.h"
#include "filters/splitter.h"
#include "logging.h"
#include "math_defs.h"
#include "opthelpers.h"
#include "polyphase_resampler.h"


namespace {

using namespace std::placeholders;

struct HrtfEntry {
    std::string mDispName;
    std::string mFilename;
};

struct LoadedHrtf {
    std::string mFilename;
    std::unique_ptr<HrtfStore> mEntry;
};

/* Data set limits must be the same as or more flexible than those defined in
 * the makemhr utility.
 */
#define MIN_IR_SIZE                  (8)
#define MAX_IR_SIZE                  (512)
#define MOD_IR_SIZE                  (2)

#define MIN_FD_COUNT                 (1)
#define MAX_FD_COUNT                 (16)

#define MIN_FD_DISTANCE              (50)
#define MAX_FD_DISTANCE              (2500)

#define MIN_EV_COUNT                 (5)
#define MAX_EV_COUNT                 (181)

#define MIN_AZ_COUNT                 (1)
#define MAX_AZ_COUNT                 (255)

#define MAX_HRIR_DELAY               (HRTF_HISTORY_LENGTH-1)

#define HRIR_DELAY_FRACBITS 2
#define HRIR_DELAY_FRACONE (1<<HRIR_DELAY_FRACBITS)
#define HRIR_DELAY_FRACHALF (HRIR_DELAY_FRACONE>>1)

static_assert(MAX_HRIR_DELAY*HRIR_DELAY_FRACONE < 256, "MAX_HRIR_DELAY or DELAY_FRAC too large");

constexpr ALchar magicMarker00[8]{'M','i','n','P','H','R','0','0'};
constexpr ALchar magicMarker01[8]{'M','i','n','P','H','R','0','1'};
constexpr ALchar magicMarker02[8]{'M','i','n','P','H','R','0','2'};

/* First value for pass-through coefficients (remaining are 0), used for omni-
 * directional sounds. */
constexpr ALfloat PassthruCoeff{0.707106781187f/*sqrt(0.5)*/};

std::mutex LoadedHrtfLock;
al::vector<LoadedHrtf> LoadedHrtfs;

std::mutex EnumeratedHrtfLock;
al::vector<HrtfEntry> EnumeratedHrtfs;


class databuf final : public std::streambuf {
    int_type underflow() override
    { return traits_type::eof(); }

    pos_type seekoff(off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode) override
    {
        if((mode&std::ios_base::out) || !(mode&std::ios_base::in))
            return traits_type::eof();

        char_type *cur;
        switch(whence)
        {
            case std::ios_base::beg:
                if(offset < 0 || offset > egptr()-eback())
                    return traits_type::eof();
                cur = eback() + offset;
                break;

            case std::ios_base::cur:
                if((offset >= 0 && offset > egptr()-gptr()) ||
                   (offset < 0 && -offset > gptr()-eback()))
                    return traits_type::eof();
                cur = gptr() + offset;
                break;

            case std::ios_base::end:
                if(offset > 0 || -offset > egptr()-eback())
                    return traits_type::eof();
                cur = egptr() + offset;
                break;

            default:
                return traits_type::eof();
        }

        setg(eback(), cur, egptr());
        return cur - eback();
    }

    pos_type seekpos(pos_type pos, std::ios_base::openmode mode) override
    {
        // Simplified version of seekoff
        if((mode&std::ios_base::out) || !(mode&std::ios_base::in))
            return traits_type::eof();

        if(pos < 0 || pos > egptr()-eback())
            return traits_type::eof();

        setg(eback(), eback() + static_cast<size_t>(pos), egptr());
        return pos;
    }

public:
    databuf(const char_type *start_, const char_type *end_) noexcept
    {
        setg(const_cast<char_type*>(start_), const_cast<char_type*>(start_),
             const_cast<char_type*>(end_));
    }
};

class idstream final : public std::istream {
    databuf mStreamBuf;

public:
    idstream(const char *start_, const char *end_)
      : std::istream{nullptr}, mStreamBuf{start_, end_}
    { init(&mStreamBuf); }
};


struct IdxBlend { ALuint idx; float blend; };
/* Calculate the elevation index given the polar elevation in radians. This
 * will return an index between 0 and (evcount - 1).
 */
IdxBlend CalcEvIndex(ALuint evcount, float ev)
{
    ev = (al::MathDefs<float>::Pi()*0.5f + ev) * static_cast<float>(evcount-1) /
        al::MathDefs<float>::Pi();
    ALuint idx{float2uint(ev)};

    return IdxBlend{minu(idx, evcount-1), ev-static_cast<float>(idx)};
}

/* Calculate the azimuth index given the polar azimuth in radians. This will
 * return an index between 0 and (azcount - 1).
 */
IdxBlend CalcAzIndex(ALuint azcount, float az)
{
    az = (al::MathDefs<float>::Tau()+az) * static_cast<float>(azcount) /
        al::MathDefs<float>::Tau();
    ALuint idx{float2uint(az)};

    return IdxBlend{idx%azcount, az-static_cast<float>(idx)};
}

} // namespace


/* Calculates static HRIR coefficients and delays for the given polar elevation
 * and azimuth in radians. The coefficients are normalized.
 */
void GetHrtfCoeffs(const HrtfStore *Hrtf, float elevation, float azimuth, float distance,
    float spread, HrirArray &coeffs, ALuint (&delays)[2])
{
    const float dirfact{1.0f - (spread / al::MathDefs<float>::Tau())};

    const auto *field = Hrtf->field;
    const auto *field_end = field + Hrtf->fdCount-1;
    size_t ebase{0};
    while(distance < field->distance && field != field_end)
    {
        ebase += field->evCount;
        ++field;
    }

    /* Claculate the elevation indinces. */
    const auto elev0 = CalcEvIndex(field->evCount, elevation);
    const size_t elev1_idx{minu(elev0.idx+1, field->evCount-1)};
    const size_t ir0offset{Hrtf->elev[ebase + elev0.idx].irOffset};
    const size_t ir1offset{Hrtf->elev[ebase + elev1_idx].irOffset};

    /* Calculate azimuth indices. */
    const auto az0 = CalcAzIndex(Hrtf->elev[ebase + elev0.idx].azCount, azimuth);
    const auto az1 = CalcAzIndex(Hrtf->elev[ebase + elev1_idx].azCount, azimuth);

    /* Calculate the HRIR indices to blend. */
    const size_t idx[4]{
        ir0offset + az0.idx,
        ir0offset + ((az0.idx+1) % Hrtf->elev[ebase + elev0.idx].azCount),
        ir1offset + az1.idx,
        ir1offset + ((az1.idx+1) % Hrtf->elev[ebase + elev1_idx].azCount)
    };

    /* Calculate bilinear blending weights, attenuated according to the
     * directional panning factor.
     */
    const float blend[4]{
        (1.0f-elev0.blend) * (1.0f-az0.blend) * dirfact,
        (1.0f-elev0.blend) * (     az0.blend) * dirfact,
        (     elev0.blend) * (1.0f-az1.blend) * dirfact,
        (     elev0.blend) * (     az1.blend) * dirfact
    };

    /* Calculate the blended HRIR delays. */
    float d{Hrtf->delays[idx[0]][0]*blend[0] + Hrtf->delays[idx[1]][0]*blend[1] +
        Hrtf->delays[idx[2]][0]*blend[2] + Hrtf->delays[idx[3]][0]*blend[3]};
    delays[0] = fastf2u(d * float{1.0f/HRIR_DELAY_FRACONE});
    d = Hrtf->delays[idx[0]][1]*blend[0] + Hrtf->delays[idx[1]][1]*blend[1] +
        Hrtf->delays[idx[2]][1]*blend[2] + Hrtf->delays[idx[3]][1]*blend[1];
    delays[1] = fastf2u(d * float{1.0f/HRIR_DELAY_FRACONE});

    const ALuint irSize{Hrtf->irSize};
    ASSUME(irSize >= MIN_IR_SIZE);

    /* Calculate the blended HRIR coefficients. */
    float *coeffout{al::assume_aligned<16>(&coeffs[0][0])};
    coeffout[0] = PassthruCoeff * (1.0f-dirfact);
    coeffout[1] = PassthruCoeff * (1.0f-dirfact);
    std::fill(coeffout+2, coeffout + HRIR_LENGTH*2, 0.0f);
    for(ALsizei c{0};c < 4;c++)
    {
        const float *srccoeffs{al::assume_aligned<16>(Hrtf->coeffs[idx[c]][0].data())};
        const float mult{blend[c]};
        auto blend_coeffs = [mult](const ALfloat src, const ALfloat coeff) noexcept -> ALfloat
        { return src*mult + coeff; };
        std::transform(srccoeffs, srccoeffs + irSize*2, coeffout, coeffout, blend_coeffs);
    }
}


std::unique_ptr<DirectHrtfState> DirectHrtfState::Create(size_t num_chans)
{
    return std::unique_ptr<DirectHrtfState>{new (FamCount{num_chans}) DirectHrtfState{num_chans}};
}

void BuildBFormatHrtf(const HrtfStore *Hrtf, DirectHrtfState *state,
    const al::span<const AngularPoint> AmbiPoints, const ALfloat (*AmbiMatrix)[MAX_AMBI_CHANNELS],
    const ALfloat *AmbiOrderHFGain)
{
    using double2 = std::array<double,2>;
    struct ImpulseResponse {
        alignas(16) std::array<double2,HRIR_LENGTH> hrir;
        ALuint ldelay, rdelay;
    };

    static const int OrderFromChan[MAX_AMBI_CHANNELS]{
        0, 1,1,1, 2,2,2,2,2, 3,3,3,3,3,3,3,
    };
    /* Set this to true for dual-band HRTF processing. May require better
     * calculation of the new IR length to deal with the head and tail
     * generated by the HF scaling.
     */
    static constexpr bool DualBand{true};

    ALuint min_delay{HRTF_HISTORY_LENGTH*HRIR_DELAY_FRACONE};
    ALuint max_delay{0};
    al::vector<ImpulseResponse> impres; impres.reserve(AmbiPoints.size());
    auto calc_res = [Hrtf,&max_delay,&min_delay](const AngularPoint &pt) -> ImpulseResponse
    {
        ImpulseResponse res;

        auto &field = Hrtf->field[0];

        /* Calculate the elevation indices. */
        const auto elev0 = CalcEvIndex(field.evCount, pt.Elev.value);
        const size_t elev1_idx{minu(elev0.idx+1, field.evCount-1)};
        const size_t ir0offset{Hrtf->elev[elev0.idx].irOffset};
        const size_t ir1offset{Hrtf->elev[elev1_idx].irOffset};

        /* Calculate azimuth indices. */
        const auto az0 = CalcAzIndex(Hrtf->elev[elev0.idx].azCount, pt.Azim.value);
        const auto az1 = CalcAzIndex(Hrtf->elev[elev1_idx].azCount, pt.Azim.value);

        /* Calculate the HRIR indices to blend. */
        const size_t idx[4]{
            ir0offset + az0.idx,
            ir0offset + ((az0.idx+1) % Hrtf->elev[elev0.idx].azCount),
            ir1offset + az1.idx,
            ir1offset + ((az1.idx+1) % Hrtf->elev[elev1_idx].azCount)};

        /* Calculate bilinear blending weights. */
        const double blend[4]{
            (1.0-elev0.blend) * (1.0-az0.blend),
            (1.0-elev0.blend) * (    az0.blend),
            (    elev0.blend) * (1.0-az1.blend),
            (    elev0.blend) * (    az1.blend)};

        /* Calculate the blended HRIR delays (in fixed-point). */
        double d{Hrtf->delays[idx[0]][0]*blend[0] + Hrtf->delays[idx[1]][0]*blend[1] +
            Hrtf->delays[idx[2]][0]*blend[2] + Hrtf->delays[idx[3]][0]*blend[3]};
        res.ldelay = fastf2u(static_cast<float>(d));
        d = Hrtf->delays[idx[0]][1]*blend[0] + Hrtf->delays[idx[1]][1]*blend[1] +
            Hrtf->delays[idx[2]][1]*blend[2] + Hrtf->delays[idx[3]][1]*blend[3];
        res.rdelay = fastf2u(static_cast<float>(d));

        /* Calculate the blended HRIR coefficients. */
        double *coeffout{al::assume_aligned<16>(&res.hrir[0][0])};
        std::fill(coeffout, coeffout + HRIR_LENGTH*2, 0.0);
        for(ALsizei c{0};c < 4;c++)
        {
            const float *srccoeffs{al::assume_aligned<16>(Hrtf->coeffs[idx[c]][0].data())};
            const double mult{blend[c]};
            auto blend_coeffs = [mult](const float src, const double coeff) noexcept -> double
            { return src*mult + coeff; };
            std::transform(srccoeffs, srccoeffs + HRIR_LENGTH*2, coeffout, coeffout, blend_coeffs);
        }

        min_delay = minu(min_delay, minu(res.ldelay, res.rdelay));
        max_delay = maxu(max_delay, maxu(res.ldelay, res.rdelay));

        return res;
    };
    std::transform(AmbiPoints.begin(), AmbiPoints.end(), std::back_inserter(impres), calc_res);
    auto hrir_delay_round = [](const ALuint d) noexcept -> ALuint
    { return (d+HRIR_DELAY_FRACHALF) >> HRIR_DELAY_FRACBITS; };

    /* For dual-band processing, add a 16-sample delay to compensate for the HF
     * scale on the minimum-phase response.
     */
    static constexpr ALuint base_delay{DualBand ? 16 : 0};
    const double xover_norm{400.0 / Hrtf->sampleRate};
    BandSplitterR<double> splitter{xover_norm};

    auto tmpres = al::vector<std::array<double2,HRIR_LENGTH>>(state->Coeffs.size());
    auto tmpflt = al::vector<std::array<double,HRIR_LENGTH*4>>(3);
    for(size_t c{0u};c < AmbiPoints.size();++c)
    {
        const al::span<const double2,HRIR_LENGTH> hrir{impres[c].hrir};
        const ALuint ldelay{hrir_delay_round(impres[c].ldelay-min_delay) + base_delay};
        const ALuint rdelay{hrir_delay_round(impres[c].rdelay-min_delay) + base_delay};

        if /*constexpr*/(!DualBand)
        {
            /* For single-band decoding, apply the HF scale to the response. */
            for(size_t i{0u};i < state->Coeffs.size();++i)
            {
                const double mult{double{AmbiOrderHFGain[OrderFromChan[i]]} * AmbiMatrix[c][i]};
                const ALuint numirs{HRIR_LENGTH - maxu(ldelay, rdelay)};
                ALuint lidx{ldelay}, ridx{rdelay};
                for(ALuint j{0};j < numirs;++j)
                {
                    tmpres[i][lidx++][0] += hrir[j][0] * mult;
                    tmpres[i][ridx++][1] += hrir[j][1] * mult;
                }
            }
            continue;
        }

        /* For dual-band processing, the HRIR needs to be split into low and
         * high frequency responses. The band-splitter alone creates frequency-
         * dependent phase-shifts, which is not ideal. To counteract it,
         * combine it with a backwards phase-shift.
         */

        /* Load the (left) HRIR backwards, into a temp buffer with padding. */
        std::fill(tmpflt[2].begin(), tmpflt[2].end(), 0.0);
        std::transform(hrir.cbegin(), hrir.cend(), tmpflt[2].rbegin() + HRIR_LENGTH*3,
            [](const double2 &ir) noexcept -> double { return ir[0]; });

        /* Apply the all-pass on the reversed signal and reverse the resulting
         * sample array. This produces the forward response with a backwards
         * phase-shift (+n degrees becomes -n degrees).
         */
        splitter.applyAllpass(tmpflt[2].data(), tmpflt[2].size());
        std::reverse(tmpflt[2].begin(), tmpflt[2].end());

        /* Now apply the band-splitter. This applies the normal phase-shift,
         * which cancels out with the backwards phase-shift to get the original
         * phase on the split signal.
         */
        splitter.clear();
        splitter.process(tmpflt[0].data(), tmpflt[1].data(), tmpflt[2].data(), tmpflt[2].size());

        /* Apply left ear response with delay and HF scale. */
        for(size_t i{0u};i < state->Coeffs.size();++i)
        {
            const ALdouble mult{AmbiMatrix[c][i]};
            const ALdouble hfgain{AmbiOrderHFGain[OrderFromChan[i]]};
            ALuint j{HRIR_LENGTH*3 - ldelay};
            for(ALuint lidx{0};lidx < HRIR_LENGTH;++lidx,++j)
                tmpres[i][lidx][0] += (tmpflt[0][j]*hfgain + tmpflt[1][j]) * mult;
        }

        /* Now run the same process on the right HRIR. */
        std::fill(tmpflt[2].begin(), tmpflt[2].end(), 0.0);
        std::transform(hrir.cbegin(), hrir.cend(), tmpflt[2].rbegin() + HRIR_LENGTH*3,
            [](const double2 &ir) noexcept -> double { return ir[1]; });

        splitter.applyAllpass(tmpflt[2].data(), tmpflt[2].size());
        std::reverse(tmpflt[2].begin(), tmpflt[2].end());

        splitter.clear();
        splitter.process(tmpflt[0].data(), tmpflt[1].data(), tmpflt[2].data(), tmpflt[2].size());

        for(size_t i{0u};i < state->Coeffs.size();++i)
        {
            const ALdouble mult{AmbiMatrix[c][i]};
            const ALdouble hfgain{AmbiOrderHFGain[OrderFromChan[i]]};
            ALuint j{HRIR_LENGTH*3 - rdelay};
            for(ALuint ridx{0};ridx < HRIR_LENGTH;++ridx,++j)
                tmpres[i][ridx][1] += (tmpflt[0][j]*hfgain + tmpflt[1][j]) * mult;
        }
    }
    tmpflt.clear();
    impres.clear();

    for(size_t i{0u};i < state->Coeffs.size();++i)
    {
        auto copy_arr = [](const double2 &in) noexcept -> float2
        { return float2{{static_cast<float>(in[0]), static_cast<float>(in[1])}}; };
        std::transform(tmpres[i].cbegin(), tmpres[i].cend(), state->Coeffs[i].begin(),
            copy_arr);
    }
    tmpres.clear();

    max_delay -= min_delay;
    ALuint max_length{HRIR_LENGTH};
    /* Increase the IR size by double the base delay with dual-band processing
     * to account for the head and tail from the HF response scale.
     */
    const ALuint irsize{minu(Hrtf->irSize + base_delay*2, max_length)};
    max_length = minu(hrir_delay_round(max_delay) + irsize, max_length);

    /* Round up to the next IR size multiple. */
    max_length += MOD_IR_SIZE-1;
    max_length -= max_length%MOD_IR_SIZE;

    TRACE("Skipped delay: %.2f, max delay: %.2f, new FIR length: %u\n",
        min_delay/double{HRIR_DELAY_FRACONE}, max_delay/double{HRIR_DELAY_FRACONE},
        max_length);
    state->IrSize = max_length;
}


namespace {

std::unique_ptr<HrtfStore> CreateHrtfStore(ALuint rate, ALushort irSize, const ALuint fdCount,
    const ALubyte *evCount, const ALushort *distance, const ALushort *azCount,
    const ALushort *irOffset, ALushort irCount, const ALfloat (*coeffs)[2],
    const ALubyte (*delays)[2], const char *filename)
{
    std::unique_ptr<HrtfStore> Hrtf;

    ALuint evTotal{std::accumulate(evCount, evCount+fdCount, 0u)};
    size_t total{sizeof(HrtfStore)};
    total  = RoundUp(total, alignof(HrtfStore::Field)); /* Align for field infos */
    total += sizeof(HrtfStore::Field)*fdCount;
    total  = RoundUp(total, alignof(HrtfStore::Elevation)); /* Align for elevation infos */
    total += sizeof(Hrtf->elev[0])*evTotal;
    total  = RoundUp(total, 16); /* Align for coefficients using SIMD */
    total += sizeof(Hrtf->coeffs[0])*irCount;
    total += sizeof(Hrtf->delays[0])*irCount;

    Hrtf.reset(new (al_calloc(16, total)) HrtfStore{});
    if(!Hrtf)
        ERR("Out of memory allocating storage for %s.\n", filename);
    else
    {
        InitRef(Hrtf->mRef, 1u);
        Hrtf->sampleRate = rate;
        Hrtf->irSize = irSize;
        Hrtf->fdCount = fdCount;

        /* Set up pointers to storage following the main HRTF struct. */
        char *base = reinterpret_cast<char*>(Hrtf.get());
        uintptr_t offset = sizeof(HrtfStore);

        offset = RoundUp(offset, alignof(HrtfStore::Field)); /* Align for field infos */
        auto field_ = reinterpret_cast<HrtfStore::Field*>(base + offset);
        offset += sizeof(field_[0])*fdCount;

        offset = RoundUp(offset, alignof(HrtfStore::Elevation)); /* Align for elevation infos */
        auto elev_ = reinterpret_cast<HrtfStore::Elevation*>(base + offset);
        offset += sizeof(elev_[0])*evTotal;

        offset = RoundUp(offset, 16); /* Align for coefficients using SIMD */
        auto coeffs_ = reinterpret_cast<HrirArray*>(base + offset);
        offset += sizeof(coeffs_[0])*irCount;

        auto delays_ = reinterpret_cast<ALubyte(*)[2]>(base + offset);
        offset += sizeof(delays_[0])*irCount;

        assert(offset == total);

        /* Copy input data to storage. */
        for(ALuint i{0};i < fdCount;i++)
        {
            field_[i].distance = distance[i] / 1000.0f;
            field_[i].evCount = evCount[i];
        }
        for(ALuint i{0};i < evTotal;i++)
        {
            elev_[i].azCount = azCount[i];
            elev_[i].irOffset = irOffset[i];
        }
        for(ALuint i{0};i < irCount;i++)
        {
            for(ALuint j{0};j < ALuint{irSize};j++)
            {
                coeffs_[i][j][0] = coeffs[i*irSize + j][0];
                coeffs_[i][j][1] = coeffs[i*irSize + j][1];
            }
            std::fill(coeffs_[i].begin()+irSize, coeffs_[i].end(), float2{});
        }
        for(ALuint i{0};i < irCount;i++)
        {
            delays_[i][0] = delays[i][0];
            delays_[i][1] = delays[i][1];
        }

        /* Finally, assign the storage pointers. */
        Hrtf->field = field_;
        Hrtf->elev = elev_;
        Hrtf->coeffs = coeffs_;
        Hrtf->delays = delays_;
    }

    return Hrtf;
}

ALubyte GetLE_ALubyte(std::istream &data)
{
    return static_cast<ALubyte>(data.get());
}

ALshort GetLE_ALshort(std::istream &data)
{
    int ret = data.get();
    ret |= data.get() << 8;
    return static_cast<ALshort>((ret^32768) - 32768);
}

ALushort GetLE_ALushort(std::istream &data)
{
    int ret = data.get();
    ret |= data.get() << 8;
    return static_cast<ALushort>(ret);
}

ALint GetLE_ALint24(std::istream &data)
{
    int ret = data.get();
    ret |= data.get() << 8;
    ret |= data.get() << 16;
    return (ret^8388608) - 8388608;
}

ALuint GetLE_ALuint(std::istream &data)
{
    int ret = data.get();
    ret |= data.get() << 8;
    ret |= data.get() << 16;
    ret |= data.get() << 24;
    return static_cast<ALuint>(ret);
}

std::unique_ptr<HrtfStore> LoadHrtf00(std::istream &data, const char *filename)
{
    ALuint rate{GetLE_ALuint(data)};
    ALushort irCount{GetLE_ALushort(data)};
    ALushort irSize{GetLE_ALushort(data)};
    ALubyte evCount{GetLE_ALubyte(data)};
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }

    ALboolean failed{AL_FALSE};
    if(irSize < MIN_IR_SIZE || irSize > MAX_IR_SIZE || (irSize%MOD_IR_SIZE))
    {
        ERR("Unsupported HRIR size: irSize=%d (%d to %d by %d)\n",
            irSize, MIN_IR_SIZE, MAX_IR_SIZE, MOD_IR_SIZE);
        failed = AL_TRUE;
    }
    if(evCount < MIN_EV_COUNT || evCount > MAX_EV_COUNT)
    {
        ERR("Unsupported elevation count: evCount=%d (%d to %d)\n",
            evCount, MIN_EV_COUNT, MAX_EV_COUNT);
        failed = AL_TRUE;
    }
    if(failed)
        return nullptr;

    auto evOffset = al::vector<ALushort>(evCount);
    for(auto &val : evOffset)
        val = GetLE_ALushort(data);
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }
    for(size_t i{1};i < evCount;i++)
    {
        if(evOffset[i] <= evOffset[i-1])
        {
            ERR("Invalid evOffset: evOffset[%zu]=%d (last=%d)\n", i, evOffset[i], evOffset[i-1]);
            failed = AL_TRUE;
        }
    }
    if(irCount <= evOffset.back())
    {
        ERR("Invalid evOffset: evOffset[%zu]=%d (irCount=%d)\n",
            evOffset.size()-1, evOffset.back(), irCount);
        failed = AL_TRUE;
    }
    if(failed)
        return nullptr;

    auto azCount = al::vector<ALushort>(evCount);
    for(size_t i{1};i < evCount;i++)
    {
        azCount[i-1] = static_cast<ALushort>(evOffset[i] - evOffset[i-1]);
        if(azCount[i-1] < MIN_AZ_COUNT || azCount[i-1] > MAX_AZ_COUNT)
        {
            ERR("Unsupported azimuth count: azCount[%zd]=%d (%d to %d)\n",
                i-1, azCount[i-1], MIN_AZ_COUNT, MAX_AZ_COUNT);
            failed = AL_TRUE;
        }
    }
    azCount.back() = static_cast<ALushort>(irCount - evOffset.back());
    if(azCount.back() < MIN_AZ_COUNT || azCount.back() > MAX_AZ_COUNT)
    {
        ERR("Unsupported azimuth count: azCount[%zu]=%d (%d to %d)\n",
            azCount.size()-1, azCount.back(), MIN_AZ_COUNT, MAX_AZ_COUNT);
        failed = AL_TRUE;
    }
    if(failed)
        return nullptr;

    auto coeffs = al::vector<std::array<ALfloat,2>>(irSize*irCount);
    auto delays = al::vector<std::array<ALubyte,2>>(irCount);
    for(auto &val : coeffs)
        val[0] = GetLE_ALshort(data) / 32768.0f;
    for(auto &val : delays)
        val[0] = GetLE_ALubyte(data);
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }
    for(size_t i{0};i < irCount;i++)
    {
        if(delays[i][0] > MAX_HRIR_DELAY)
        {
            ERR("Invalid delays[%zd]: %d (%d)\n", i, delays[i][0], MAX_HRIR_DELAY);
            failed = AL_TRUE;
        }
        delays[i][0] <<= HRIR_DELAY_FRACBITS;
    }
    if(failed)
        return nullptr;

    /* Mirror the left ear responses to the right ear. */
    for(size_t i{0};i < evCount;i++)
    {
        const ALushort evoffset{evOffset[i]};
        const ALushort azcount{azCount[i]};
        for(size_t j{0};j < azcount;j++)
        {
            const size_t lidx{evoffset + j};
            const size_t ridx{evoffset + ((azcount-j) % azcount)};

            for(size_t k{0};k < irSize;k++)
                coeffs[ridx*irSize + k][1] = coeffs[lidx*irSize + k][0];
            delays[ridx][1] = delays[lidx][0];
        }
    }

    static const ALushort distance{0};
    return CreateHrtfStore(rate, irSize, 1, &evCount, &distance, azCount.data(), evOffset.data(),
        irCount, &reinterpret_cast<ALfloat(&)[2]>(coeffs[0]),
        &reinterpret_cast<ALubyte(&)[2]>(delays[0]), filename);
}

std::unique_ptr<HrtfStore> LoadHrtf01(std::istream &data, const char *filename)
{
    ALuint rate{GetLE_ALuint(data)};
    ALushort irSize{GetLE_ALubyte(data)};
    ALubyte evCount{GetLE_ALubyte(data)};
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }

    ALboolean failed{AL_FALSE};
    if(irSize < MIN_IR_SIZE || irSize > MAX_IR_SIZE || (irSize%MOD_IR_SIZE))
    {
        ERR("Unsupported HRIR size: irSize=%d (%d to %d by %d)\n",
            irSize, MIN_IR_SIZE, MAX_IR_SIZE, MOD_IR_SIZE);
        failed = AL_TRUE;
    }
    if(evCount < MIN_EV_COUNT || evCount > MAX_EV_COUNT)
    {
        ERR("Unsupported elevation count: evCount=%d (%d to %d)\n",
            evCount, MIN_EV_COUNT, MAX_EV_COUNT);
        failed = AL_TRUE;
    }
    if(failed)
        return nullptr;

    auto azCount = al::vector<ALushort>(evCount);
    std::generate(azCount.begin(), azCount.end(), std::bind(GetLE_ALubyte, std::ref(data)));
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }
    for(size_t i{0};i < evCount;++i)
    {
        if(azCount[i] < MIN_AZ_COUNT || azCount[i] > MAX_AZ_COUNT)
        {
            ERR("Unsupported azimuth count: azCount[%zd]=%d (%d to %d)\n", i, azCount[i],
                MIN_AZ_COUNT, MAX_AZ_COUNT);
            failed = AL_TRUE;
        }
    }
    if(failed)
        return nullptr;

    auto evOffset = al::vector<ALushort>(evCount);
    evOffset[0] = 0;
    ALushort irCount{azCount[0]};
    for(size_t i{1};i < evCount;i++)
    {
        evOffset[i] = static_cast<ALushort>(evOffset[i-1] + azCount[i-1]);
        irCount = static_cast<ALushort>(irCount + azCount[i]);
    }

    auto coeffs = al::vector<std::array<ALfloat,2>>(irSize*irCount);
    auto delays = al::vector<std::array<ALubyte,2>>(irCount);
    for(auto &val : coeffs)
        val[0] = GetLE_ALshort(data) / 32768.0f;
    for(auto &val : delays)
        val[0] = GetLE_ALubyte(data);
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }
    for(size_t i{0};i < irCount;i++)
    {
        if(delays[i][0] > MAX_HRIR_DELAY)
        {
            ERR("Invalid delays[%zd]: %d (%d)\n", i, delays[i][0], MAX_HRIR_DELAY);
            failed = AL_TRUE;
        }
        delays[i][0] <<= HRIR_DELAY_FRACBITS;
    }
    if(failed)
        return nullptr;

    /* Mirror the left ear responses to the right ear. */
    for(size_t i{0};i < evCount;i++)
    {
        const ALushort evoffset{evOffset[i]};
        const ALushort azcount{azCount[i]};
        for(size_t j{0};j < azcount;j++)
        {
            const size_t lidx{evoffset + j};
            const size_t ridx{evoffset + ((azcount-j) % azcount)};

            for(size_t k{0};k < irSize;k++)
                coeffs[ridx*irSize + k][1] = coeffs[lidx*irSize + k][0];
            delays[ridx][1] = delays[lidx][0];
        }
    }

    static const ALushort distance{0};
    return CreateHrtfStore(rate, irSize, 1, &evCount, &distance, azCount.data(), evOffset.data(),
        irCount, &reinterpret_cast<ALfloat(&)[2]>(coeffs[0]),
        &reinterpret_cast<ALubyte(&)[2]>(delays[0]), filename);
}

#define SAMPLETYPE_S16 0
#define SAMPLETYPE_S24 1

#define CHANTYPE_LEFTONLY  0
#define CHANTYPE_LEFTRIGHT 1

std::unique_ptr<HrtfStore> LoadHrtf02(std::istream &data, const char *filename)
{
    ALuint rate{GetLE_ALuint(data)};
    ALubyte sampleType{GetLE_ALubyte(data)};
    ALubyte channelType{GetLE_ALubyte(data)};
    ALushort irSize{GetLE_ALubyte(data)};
    ALubyte fdCount{GetLE_ALubyte(data)};
    if(!data || data.eof())
    {
        ERR("Failed reading %s\n", filename);
        return nullptr;
    }

    ALboolean failed{AL_FALSE};
    if(sampleType > SAMPLETYPE_S24)
    {
        ERR("Unsupported sample type: %d\n", sampleType);
        failed = AL_TRUE;
    }
    if(channelType > CHANTYPE_LEFTRIGHT)
    {
        ERR("Unsupported channel type: %d\n", channelType);
        failed = AL_TRUE;
    }

    if(irSize < MIN_IR_SIZE || irSize > MAX_IR_SIZE || (irSize%MOD_IR_SIZE))
    {
        ERR("Unsupported HRIR size: irSize=%d (%d to %d by %d)\n",
            irSize, MIN_IR_SIZE, MAX_IR_SIZE, MOD_IR_SIZE);
        failed = AL_TRUE;
    }
    if(fdCount < 1 || fdCount > MAX_FD_COUNT)
    {
        ERR("Multiple field-depths not supported: fdCount=%d (%d to %d)\n",
            fdCount, MIN_FD_COUNT, MAX_FD_COUNT);
        failed = AL_TRUE;
    }
    if(failed)
        return nullptr;

    auto distance = al::vector<ALushort>(fdCount);
    auto evCount = al::vector<ALubyte>(fdCount);
    auto azCount = al::vector<ALushort>{};
    for(size_t f{0};f < fdCount;f++)
    {
        distance[f] = GetLE_ALushort(data);
        evCount[f] = GetLE_ALubyte(data);
        if(!data || data.eof())
        {
            ERR("Failed reading %s\n", filename);
            return nullptr;
        }

        if(distance[f] < MIN_FD_DISTANCE || distance[f] > MAX_FD_DISTANCE)
        {
            ERR("Unsupported field distance[%zu]=%d (%d to %d millimeters)\n", f, distance[f],
                MIN_FD_DISTANCE, MAX_FD_DISTANCE);
            failed = AL_TRUE;
        }
        if(f > 0 && distance[f] <= distance[f-1])
        {
            ERR("Field distance[%zu] is not after previous (%d > %d)\n", f, distance[f],
                distance[f-1]);
            failed = AL_TRUE;
        }
        if(evCount[f] < MIN_EV_COUNT || evCount[f] > MAX_EV_COUNT)
        {
            ERR("Unsupported elevation count: evCount[%zu]=%d (%d to %d)\n", f, evCount[f],
                MIN_EV_COUNT, MAX_EV_COUNT);
            failed = AL_TRUE;
        }
        if(failed)
            return nullptr;

        const size_t ebase{azCount.size()};
        azCount.resize(ebase + evCount[f]);
        std::generate(azCount.begin()+static_cast<ptrdiff_t>(ebase), azCount.end(),
            std::bind(GetLE_ALubyte, std::ref(data)));
        if(!data || data.eof())
        {
            ERR("Failed reading %s\n", filename);
            return nullptr;
        }

        for(size_t e{0};e < evCount[f];e++)
        {
            if(azCount[ebase+e] < MIN_AZ_COUNT || azCount[ebase+e] > MAX_AZ_COUNT)
            {
                ERR("Unsupported azimuth count: azCount[%zu][%zu]=%d (%d to %d)\n", f, e,
                    azCount[ebase+e], MIN_AZ_COUNT, MAX_AZ_COUNT);
                failed = AL_TRUE;
            }
        }
        if(failed)
            return nullptr;
    }

    auto evOffset = al::vector<ALushort>(azCount.size());
    evOffset[0] = 0;
    std::partial_sum(azCount.cbegin(), azCount.cend()-1, evOffset.begin()+1);
    const auto irTotal = static_cast<ALushort>(evOffset.back() + azCount.back());

    auto coeffs = al::vector<std::array<ALfloat,2>>(irSize*irTotal);
    auto delays = al::vector<std::array<ALubyte,2>>(irTotal);
    if(channelType == CHANTYPE_LEFTONLY)
    {
        if(sampleType == SAMPLETYPE_S16)
        {
            for(auto &val : coeffs)
                val[0] = GetLE_ALshort(data) / 32768.0f;
        }
        else if(sampleType == SAMPLETYPE_S24)
        {
            for(auto &val : coeffs)
                val[0] = static_cast<float>(GetLE_ALint24(data)) / 8388608.0f;
        }
        for(auto &val : delays)
            val[0] = GetLE_ALubyte(data);
        if(!data || data.eof())
        {
            ERR("Failed reading %s\n", filename);
            return nullptr;
        }
        for(size_t i{0};i < irTotal;++i)
        {
            if(delays[i][0] > MAX_HRIR_DELAY)
            {
                ERR("Invalid delays[%zu][0]: %d (%d)\n", i, delays[i][0], MAX_HRIR_DELAY);
                failed = AL_TRUE;
            }
            delays[i][0] <<= HRIR_DELAY_FRACBITS;
        }
    }
    else if(channelType == CHANTYPE_LEFTRIGHT)
    {
        if(sampleType == SAMPLETYPE_S16)
        {
            for(auto &val : coeffs)
            {
                val[0] = GetLE_ALshort(data) / 32768.0f;
                val[1] = GetLE_ALshort(data) / 32768.0f;
            }
        }
        else if(sampleType == SAMPLETYPE_S24)
        {
            for(auto &val : coeffs)
            {
                val[0] = static_cast<float>(GetLE_ALint24(data)) / 8388608.0f;
                val[1] = static_cast<float>(GetLE_ALint24(data)) / 8388608.0f;
            }
        }
        for(auto &val : delays)
        {
            val[0] = GetLE_ALubyte(data);
            val[1] = GetLE_ALubyte(data);
        }
        if(!data || data.eof())
        {
            ERR("Failed reading %s\n", filename);
            return nullptr;
        }

        for(size_t i{0};i < irTotal;++i)
        {
            if(delays[i][0] > MAX_HRIR_DELAY)
            {
                ERR("Invalid delays[%zu][0]: %d (%d)\n", i, delays[i][0], MAX_HRIR_DELAY);
                failed = AL_TRUE;
            }
            if(delays[i][1] > MAX_HRIR_DELAY)
            {
                ERR("Invalid delays[%zu][1]: %d (%d)\n", i, delays[i][1], MAX_HRIR_DELAY);
                failed = AL_TRUE;
            }
            delays[i][0] <<= HRIR_DELAY_FRACBITS;
            delays[i][1] <<= HRIR_DELAY_FRACBITS;
        }
    }
    if(failed)
        return nullptr;

    if(channelType == CHANTYPE_LEFTONLY)
    {
        /* Mirror the left ear responses to the right ear. */
        size_t ebase{0};
        for(size_t f{0};f < fdCount;f++)
        {
            for(size_t e{0};e < evCount[f];e++)
            {
                const ALushort evoffset{evOffset[ebase+e]};
                const ALushort azcount{azCount[ebase+e]};
                for(size_t a{0};a < azcount;a++)
                {
                    const size_t lidx{evoffset + a};
                    const size_t ridx{evoffset + ((azcount-a) % azcount)};

                    for(size_t k{0};k < irSize;k++)
                        coeffs[ridx*irSize + k][1] = coeffs[lidx*irSize + k][0];
                    delays[ridx][1] = delays[lidx][0];
                }
            }
            ebase += evCount[f];
        }
    }

    if(fdCount > 1)
    {
        auto distance_ = al::vector<ALushort>(distance.size());
        auto evCount_ = al::vector<ALubyte>(evCount.size());
        auto azCount_ = al::vector<ALushort>(azCount.size());
        auto evOffset_ = al::vector<ALushort>(evOffset.size());
        auto coeffs_ = al::vector<float2>(coeffs.size());
        auto delays_ = al::vector<std::array<ALubyte,2>>(delays.size());

        /* Simple reverse for the per-field elements. */
        std::reverse_copy(distance.cbegin(), distance.cend(), distance_.begin());
        std::reverse_copy(evCount.cbegin(), evCount.cend(), evCount_.begin());

        /* Each field has a group of elevations, which each have an azimuth
         * count. Reverse the order of the groups, keeping the relative order
         * of per-group azimuth counts.
         */
        auto azcnt_end = azCount_.end();
        auto copy_azs = [&azCount,&azcnt_end](const ptrdiff_t ebase, const ALubyte num_evs) -> ptrdiff_t
        {
            auto azcnt_src = azCount.begin()+ebase;
            azcnt_end = std::copy_backward(azcnt_src, azcnt_src+num_evs, azcnt_end);
            return ebase + num_evs;
        };
        std::accumulate(evCount.cbegin(), evCount.cend(), ptrdiff_t{0}, copy_azs);
        assert(azCount_.begin() == azcnt_end);

        /* Reestablish the IR offset for each elevation index, given the new
         * ordering of elevations.
         */
        evOffset_[0] = 0;
        std::partial_sum(azCount_.cbegin(), azCount_.cend()-1, evOffset_.begin()+1);

        /* Reverse the order of each field's group of IRs. */
        auto coeffs_end = coeffs_.end();
        auto delays_end = delays_.end();
        auto copy_irs = [irSize,&azCount,&coeffs,&delays,&coeffs_end,&delays_end](const ptrdiff_t ebase, const ALubyte num_evs) -> ptrdiff_t
        {
            const ALsizei abase{std::accumulate(azCount.cbegin(), azCount.cbegin()+ebase, 0)};
            const ALsizei num_azs{std::accumulate(azCount.cbegin()+ebase,
                azCount.cbegin() + (ebase+num_evs), 0)};

            coeffs_end = std::copy_backward(coeffs.cbegin() + abase*irSize,
                coeffs.cbegin() + (abase+num_azs)*irSize, coeffs_end);
            delays_end = std::copy_backward(delays.cbegin() + abase,
                delays.cbegin() + (abase+num_azs), delays_end);

            return ebase + num_evs;
        };
        std::accumulate(evCount.cbegin(), evCount.cend(), ptrdiff_t{0}, copy_irs);
        assert(coeffs_.begin() == coeffs_end);
        assert(delays_.begin() == delays_end);

        distance = std::move(distance_);
        evCount = std::move(evCount_);
        azCount = std::move(azCount_);
        evOffset = std::move(evOffset_);
        coeffs = std::move(coeffs_);
        delays = std::move(delays_);
    }

    return CreateHrtfStore(rate, irSize, fdCount, evCount.data(), distance.data(), azCount.data(),
        evOffset.data(), irTotal, &reinterpret_cast<ALfloat(&)[2]>(coeffs[0]),
        &reinterpret_cast<ALubyte(&)[2]>(delays[0]), filename);
}


bool checkName(const std::string &name)
{
    auto match_name = [&name](const HrtfEntry &entry) -> bool { return name == entry.mDispName; };
    auto &enum_names = EnumeratedHrtfs;
    return std::find_if(enum_names.cbegin(), enum_names.cend(), match_name) != enum_names.cend();
}

void AddFileEntry(const std::string &filename)
{
    /* Check if this file has already been enumerated. */
    auto enum_iter = std::find_if(EnumeratedHrtfs.cbegin(), EnumeratedHrtfs.cend(),
        [&filename](const HrtfEntry &entry) -> bool
        { return entry.mFilename == filename; });
    if(enum_iter != EnumeratedHrtfs.cend())
    {
        TRACE("Skipping duplicate file entry %s\n", filename.c_str());
        return;
    }

    /* TODO: Get a human-readable name from the HRTF data (possibly coming in a
     * format update). */
    size_t namepos = filename.find_last_of('/')+1;
    if(!namepos) namepos = filename.find_last_of('\\')+1;

    size_t extpos{filename.find_last_of('.')};
    if(extpos <= namepos) extpos = std::string::npos;

    const std::string basename{(extpos == std::string::npos) ?
        filename.substr(namepos) : filename.substr(namepos, extpos-namepos)};
    std::string newname{basename};
    int count{1};
    while(checkName(newname))
    {
        newname = basename;
        newname += " #";
        newname += std::to_string(++count);
    }
    EnumeratedHrtfs.emplace_back(HrtfEntry{newname, filename});
    const HrtfEntry &entry = EnumeratedHrtfs.back();

    TRACE("Adding file entry \"%s\"\n", entry.mFilename.c_str());
}

/* Unfortunate that we have to duplicate AddFileEntry to take a memory buffer
 * for input instead of opening the given filename.
 */
void AddBuiltInEntry(const std::string &dispname, ALuint residx)
{
    const std::string filename{'!'+std::to_string(residx)+'_'+dispname};

    auto enum_iter = std::find_if(EnumeratedHrtfs.cbegin(), EnumeratedHrtfs.cend(),
        [&filename](const HrtfEntry &entry) -> bool
        { return entry.mFilename == filename; });
    if(enum_iter != EnumeratedHrtfs.cend())
    {
        TRACE("Skipping duplicate file entry %s\n", filename.c_str());
        return;
    }

    /* TODO: Get a human-readable name from the HRTF data (possibly coming in a
     * format update). */

    std::string newname{dispname};
    int count{1};
    while(checkName(newname))
    {
        newname = dispname;
        newname += " #";
        newname += std::to_string(++count);
    }
    EnumeratedHrtfs.emplace_back(HrtfEntry{newname, filename});
    const HrtfEntry &entry = EnumeratedHrtfs.back();

    TRACE("Adding built-in entry \"%s\"\n", entry.mFilename.c_str());
}


#define IDR_DEFAULT_HRTF_MHR 1

#ifndef ALSOFT_EMBED_HRTF_DATA

al::span<const char> GetResource(int /*name*/)
{ return {}; }

#else

#include "hrtf_default.h"

al::span<const char> GetResource(int name)
{
    if(name == IDR_DEFAULT_HRTF_MHR)
        return {reinterpret_cast<const char*>(hrtf_default), sizeof(hrtf_default)};
    return {};
}
#endif

} // namespace


al::vector<std::string> EnumerateHrtf(const char *devname)
{
    std::lock_guard<std::mutex> _{EnumeratedHrtfLock};
    EnumeratedHrtfs.clear();

    bool usedefaults{true};
    if(auto pathopt = ConfigValueStr(devname, nullptr, "hrtf-paths"))
    {
        const char *pathlist{pathopt->c_str()};
        while(pathlist && *pathlist)
        {
            const char *next, *end;

            while(isspace(*pathlist) || *pathlist == ',')
                pathlist++;
            if(*pathlist == '\0')
                continue;

            next = strchr(pathlist, ',');
            if(next)
                end = next++;
            else
            {
                end = pathlist + strlen(pathlist);
                usedefaults = false;
            }

            while(end != pathlist && isspace(*(end-1)))
                --end;
            if(end != pathlist)
            {
                const std::string pname{pathlist, end};
                for(const auto &fname : SearchDataFiles(".mhr", pname.c_str()))
                    AddFileEntry(fname);
            }

            pathlist = next;
        }
    }

    if(usedefaults)
    {
        for(const auto &fname : SearchDataFiles(".mhr", "openal/hrtf"))
            AddFileEntry(fname);

        if(!GetResource(IDR_DEFAULT_HRTF_MHR).empty())
            AddBuiltInEntry("Built-In HRTF", IDR_DEFAULT_HRTF_MHR);
    }

    al::vector<std::string> list;
    list.reserve(EnumeratedHrtfs.size());
    for(auto &entry : EnumeratedHrtfs)
        list.emplace_back(entry.mDispName);

    if(auto defhrtfopt = ConfigValueStr(devname, nullptr, "default-hrtf"))
    {
        auto iter = std::find(list.begin(), list.end(), *defhrtfopt);
        if(iter == list.end())
            WARN("Failed to find default HRTF \"%s\"\n", defhrtfopt->c_str());
        else if(iter != list.begin())
            std::rotate(list.begin(), iter, iter+1);
    }

    return list;
}

HrtfStore *GetLoadedHrtf(const std::string &name, const char *devname, const ALuint devrate)
{
    std::lock_guard<std::mutex> _{EnumeratedHrtfLock};
    auto entry_iter = std::find_if(EnumeratedHrtfs.cbegin(), EnumeratedHrtfs.cend(),
        [&name](const HrtfEntry &entry) -> bool { return entry.mDispName == name; }
    );
    if(entry_iter == EnumeratedHrtfs.cend())
        return nullptr;
    const std::string &fname = entry_iter->mFilename;

    std::lock_guard<std::mutex> __{LoadedHrtfLock};
    auto hrtf_lt_fname = [](LoadedHrtf &hrtf, const std::string &filename) -> bool
    { return hrtf.mFilename < filename; };
    auto handle = std::lower_bound(LoadedHrtfs.begin(), LoadedHrtfs.end(), fname, hrtf_lt_fname);
    while(handle != LoadedHrtfs.end() && handle->mFilename == fname)
    {
        HrtfStore *hrtf{handle->mEntry.get()};
        if(hrtf && hrtf->sampleRate == devrate)
        {
            hrtf->IncRef();
            return hrtf;
        }
        ++handle;
    }

    std::unique_ptr<std::istream> stream;
    ALint residx{};
    char ch{};
    if(sscanf(fname.c_str(), "!%d%c", &residx, &ch) == 2 && ch == '_')
    {
        TRACE("Loading %s...\n", fname.c_str());
        al::span<const char> res{GetResource(residx)};
        if(res.empty())
        {
            ERR("Could not get resource %u, %s\n", residx, name.c_str());
            return nullptr;
        }
        stream = al::make_unique<idstream>(res.begin(), res.end());
    }
    else
    {
        TRACE("Loading %s...\n", fname.c_str());
        auto fstr = al::make_unique<al::ifstream>(fname.c_str(), std::ios::binary);
        if(!fstr->is_open())
        {
            ERR("Could not open %s\n", fname.c_str());
            return nullptr;
        }
        stream = std::move(fstr);
    }

    std::unique_ptr<HrtfStore> hrtf;
    char magic[sizeof(magicMarker02)];
    stream->read(magic, sizeof(magic));
    if(stream->gcount() < static_cast<std::streamsize>(sizeof(magicMarker02)))
        ERR("%s data is too short (%zu bytes)\n", name.c_str(), stream->gcount());
    else if(memcmp(magic, magicMarker02, sizeof(magicMarker02)) == 0)
    {
        TRACE("Detected data set format v2\n");
        hrtf = LoadHrtf02(*stream, name.c_str());
    }
    else if(memcmp(magic, magicMarker01, sizeof(magicMarker01)) == 0)
    {
        TRACE("Detected data set format v1\n");
        hrtf = LoadHrtf01(*stream, name.c_str());
    }
    else if(memcmp(magic, magicMarker00, sizeof(magicMarker00)) == 0)
    {
        TRACE("Detected data set format v0\n");
        hrtf = LoadHrtf00(*stream, name.c_str());
    }
    else
        ERR("Invalid header in %s: \"%.8s\"\n", name.c_str(), magic);
    stream.reset();

    if(!hrtf)
    {
        ERR("Failed to load %s\n", name.c_str());
        return nullptr;
    }

    if(hrtf->sampleRate != devrate)
    {
        /* Calculate the last elevation's index and get the total IR count. */
        const size_t lastEv{std::accumulate(hrtf->field, hrtf->field+hrtf->fdCount, size_t{0},
            [](const size_t curval, const HrtfStore::Field &field) noexcept -> size_t
            { return curval + field.evCount; }
        ) - 1};
        const size_t irCount{size_t{hrtf->elev[lastEv].irOffset} + hrtf->elev[lastEv].azCount};

        /* Resample all the IRs. */
        std::array<std::array<double,HRIR_LENGTH>,2> inout;
        PPhaseResampler rs;
        rs.init(hrtf->sampleRate, devrate);
        for(size_t i{0};i < irCount;++i)
        {
            HrirArray &coeffs = const_cast<HrirArray&>(hrtf->coeffs[i]);
            for(size_t j{0};j < 2;++j)
            {
                std::transform(coeffs.cbegin(), coeffs.cend(), inout[0].begin(),
                    [j](const float2 &in) noexcept -> double { return in[j]; });
                rs.process(HRIR_LENGTH, inout[0].data(), HRIR_LENGTH, inout[1].data());
                for(size_t k{0};k < HRIR_LENGTH;++k)
                    coeffs[k][j] = static_cast<float>(inout[1][k]);
            }
        }
        rs = {};

        const ALuint srate{hrtf->sampleRate};
        for(size_t i{0};i < irCount;++i)
        {
            for(ALubyte &delay : const_cast<ALubyte(&)[2]>(hrtf->delays[i]))
                delay = static_cast<ALubyte>(minu64(MAX_HRIR_DELAY*HRIR_DELAY_FRACONE,
                    (uint64_t{delay}*devrate + srate/2) / srate));
        }

        /* Scale the IR size for the new sample rate and update the stored
         * sample rate.
         */
        uint64_t newIrSize{(uint64_t{hrtf->irSize}*devrate + srate-1) / srate};
        newIrSize = minu64(HRIR_LENGTH, newIrSize) + (MOD_IR_SIZE-1);
        hrtf->irSize = static_cast<ALuint>(newIrSize - (newIrSize%MOD_IR_SIZE));
        hrtf->sampleRate = devrate;
    }

    if(auto hrtfsizeopt = ConfigValueUInt(devname, nullptr, "hrtf-size"))
    {
        if(*hrtfsizeopt > 0 && *hrtfsizeopt < hrtf->irSize)
        {
            hrtf->irSize  = maxu(*hrtfsizeopt, MIN_IR_SIZE);
            hrtf->irSize -= hrtf->irSize % MOD_IR_SIZE;
        }
    }

    TRACE("Loaded HRTF %s for sample rate %uhz, %u-sample filter\n", name.c_str(),
        hrtf->sampleRate, hrtf->irSize);
    handle = LoadedHrtfs.emplace(handle, LoadedHrtf{fname, std::move(hrtf)});

    return handle->mEntry.get();
}


void HrtfStore::IncRef()
{
    auto ref = IncrementRef(mRef);
    TRACE("HrtfEntry %p increasing refcount to %u\n", decltype(std::declval<void*>()){this}, ref);
}

void HrtfStore::DecRef()
{
    auto ref = DecrementRef(mRef);
    TRACE("HrtfEntry %p decreasing refcount to %u\n", decltype(std::declval<void*>()){this}, ref);
    if(ref == 0)
    {
        std::lock_guard<std::mutex> _{LoadedHrtfLock};

        /* Go through and remove all unused HRTFs. */
        auto remove_unused = [](LoadedHrtf &hrtf) -> bool
        {
            HrtfStore *entry{hrtf.mEntry.get()};
            if(entry && ReadRef(entry->mRef) == 0)
            {
                TRACE("Unloading unused HRTF %s\n", hrtf.mFilename.data());
                hrtf.mEntry = nullptr;
                return true;
            }
            return false;
        };
        auto iter = std::remove_if(LoadedHrtfs.begin(), LoadedHrtfs.end(), remove_unused);
        LoadedHrtfs.erase(iter, LoadedHrtfs.end());
    }
}
