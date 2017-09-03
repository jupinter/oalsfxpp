#ifndef _ALU_H_
#define _ALU_H_

#include "alMain.h"
#include "alBuffer.h"
#include "alFilter.h"
#include "alAuxEffectSlot.h"


struct ALsource;
struct ALvoice;
struct ALeffectslot;


union aluMatrixf
{
    ALfloat m[4][4];
}; // aluMatrixf

extern const aluMatrixf IdentityMatrixf;

inline void aluMatrixfSetRow(aluMatrixf *matrix, ALuint row,
                             ALfloat m0, ALfloat m1, ALfloat m2, ALfloat m3)
{
    matrix->m[row][0] = m0;
    matrix->m[row][1] = m1;
    matrix->m[row][2] = m2;
    matrix->m[row][3] = m3;
}

inline void aluMatrixfSet(aluMatrixf *matrix, ALfloat m00, ALfloat m01, ALfloat m02, ALfloat m03,
                                              ALfloat m10, ALfloat m11, ALfloat m12, ALfloat m13,
                                              ALfloat m20, ALfloat m21, ALfloat m22, ALfloat m23,
                                              ALfloat m30, ALfloat m31, ALfloat m32, ALfloat m33)
{
    aluMatrixfSetRow(matrix, 0, m00, m01, m02, m03);
    aluMatrixfSetRow(matrix, 1, m10, m11, m12, m13);
    aluMatrixfSetRow(matrix, 2, m20, m21, m22, m23);
    aluMatrixfSetRow(matrix, 3, m30, m31, m32, m33);
}


enum ActiveFilters {
    AF_None = 0,
    AF_LowPass = 1,
    AF_HighPass = 2,
    AF_BandPass = AF_LowPass | AF_HighPass
};


struct DirectParams
{
    struct Gains
    {
        ALfloat current[MAX_OUTPUT_CHANNELS];
        ALfloat target[MAX_OUTPUT_CHANNELS];
    }; // Gains


    ALfilterState low_pass;
    ALfilterState high_pass;
    Gains gains;
}; // DirectParams

struct SendParams
{
    struct Gains
    {
        ALfloat current[MAX_OUTPUT_CHANNELS];
        ALfloat target[MAX_OUTPUT_CHANNELS];
    }; // Gains


    ALfilterState low_pass;
    ALfilterState high_pass;
    Gains gains;
}; // SendParams


struct ALvoiceProps
{
    struct Direct
    {
        ALfloat gain;
        ALfloat gain_hf;
        ALfloat hf_reference;
        ALfloat gain_lf;
        ALfloat lf_reference;
    }; // Direct

    struct Send
    {
        struct ALeffectslot *slot;
        ALfloat gain;
        ALfloat gain_hf;
        ALfloat hf_reference;
        ALfloat gain_lf;
        ALfloat lf_reference;
    }; // Send

    struct ALvoiceProps* next;
    ALfloat stereo_pan[2];
    ALfloat radius;

    // Direct filter and auxiliary send info.
    Direct direct;
    Send send[1];
};

struct ALvoice
{
    struct Direct
    {
        ActiveFilters filter_type;
        DirectParams params[MAX_INPUT_CHANNELS];
        SampleBuffers* buffer;
        ALsizei channels;
        ALsizei channels_per_order[MAX_AMBI_ORDER + 1];
    }; // Direct

    struct Send
    {
        ActiveFilters filter_type;
        SendParams params[MAX_INPUT_CHANNELS];
        SampleBuffers* buffer;
        ALsizei channels;
    }; // Send


    struct ALvoiceProps *props;
    struct ALsource* source;
    bool playing;

    // Number of channels and bytes-per-sample for the attached source's
    // buffer(s).
    ALsizei num_channels;

    Direct direct;
    Send send[1];
}; // ALvoice

void DeinitVoice(ALvoice *voice);


using MixerFunc = void (*)(const ALfloat *data, ALsizei OutChans,
                          SampleBuffers& OutBuffer, ALfloat *CurrentGains,
                          const ALfloat *TargetGains, ALsizei Counter, ALsizei OutPos,
                          ALsizei BufferSize);

using RowMixerFunc = void (*)(ALfloat *OutBuffer, const ALfloat *gains,
                             const SampleBuffers& data, ALsizei InChans,
                             ALsizei InPos, ALsizei BufferSize);


constexpr auto GAIN_MIX_MAX = 16.0F; /* +24dB */

constexpr auto GAIN_SILENCE_THRESHOLD = 0.00001F; /* -100dB */

constexpr auto SPEEDOFSOUNDMETRESPERSEC = 343.3F;

/* Target gain for the reverb decay feedback reaching the decay time. */
constexpr auto REVERB_DECAY_GAIN = 0.001F; /* -60 dB */


inline ALfloat minf(ALfloat a, ALfloat b)
{ return ((a > b) ? b : a); }
inline ALfloat maxf(ALfloat a, ALfloat b)
{ return ((a > b) ? a : b); }
inline ALfloat clampf(ALfloat val, ALfloat min, ALfloat max)
{ return minf(max, maxf(min, val)); }

inline ALuint minu(ALuint a, ALuint b)
{ return ((a > b) ? b : a); }
inline ALuint maxu(ALuint a, ALuint b)
{ return ((a > b) ? a : b); }
inline ALuint clampu(ALuint val, ALuint min, ALuint max)
{ return minu(max, maxu(min, val)); }

inline ALint mini(ALint a, ALint b)
{ return ((a > b) ? b : a); }
inline ALint maxi(ALint a, ALint b)
{ return ((a > b) ? a : b); }
inline ALint clampi(ALint val, ALint min, ALint max)
{ return mini(max, maxi(min, val)); }


inline ALfloat lerp(ALfloat val1, ALfloat val2, ALfloat mu)
{
    return val1 + (val2-val1)*mu;
}


/* aluInitRenderer
 *
 * Set up the appropriate panning method and mixing method given the device
 * properties.
 */
void aluInitRenderer(ALCdevice *device);

void aluInitEffectPanning(struct ALeffectslot *slot);

/**
 * CalcDirectionCoeffs
 *
 * Calculates ambisonic coefficients based on a direction vector. The vector
 * must be normalized (unit length), and the spread is the angular width of the
 * sound (0...tau).
 */
void CalcDirectionCoeffs(const ALfloat dir[3], ALfloat spread, ALfloat coeffs[MAX_AMBI_COEFFS]);

/**
 * CalcAngleCoeffs
 *
 * Calculates ambisonic coefficients based on azimuth and elevation. The
 * azimuth and elevation parameters are in radians, going right and up
 * respectively.
 */
inline void CalcAngleCoeffs(ALfloat azimuth, ALfloat elevation, ALfloat spread, ALfloat coeffs[MAX_AMBI_COEFFS])
{
    ALfloat dir[3] = {
        sinf(azimuth) * cosf(elevation),
        sinf(elevation),
        -cosf(azimuth) * cosf(elevation)
    };
    CalcDirectionCoeffs(dir, spread, coeffs);
}

/**
 * ComputeAmbientGains
 *
 * Computes channel gains for ambient, omni-directional sounds.
 */
template<typename T>
void ComputeAmbientGains(
    const T& b,
    const ALfloat g,
    ALfloat* const o)
{
    if(b.coeff_count > 0)
    {
        ComputeAmbientGainsMC(b.ambi.coeffs, b.num_channels, g, o);
    }
    else
    {
        ComputeAmbientGainsBF(b.ambi.map, b.num_channels, g, o);
    }
}

void ComputeAmbientGainsMC(const ChannelConfig *chancoeffs, ALsizei numchans, ALfloat ingain, ALfloat gains[MAX_OUTPUT_CHANNELS]);
void ComputeAmbientGainsBF(const BFChannelConfig *chanmap, ALsizei numchans, ALfloat ingain, ALfloat gains[MAX_OUTPUT_CHANNELS]);

/**
 * ComputePanningGains
 *
 * Computes panning gains using the given channel decoder coefficients and the
 * pre-calculated direction or angle coefficients.
 */
template<typename T>
void ComputePanningGains(
    const T& b,
    const ALfloat* const c,
    const ALfloat g,
    ALfloat* const o)
{
    if (b.coeff_count > 0)
    {
        ComputePanningGainsMC(b.ambi.coeffs, b.num_channels, b.coeff_count, c, g, o);
    }
    else
    {
        ComputePanningGainsBF(b.ambi.map, b.num_channels, c, g, o);
    }
}

void ComputePanningGainsMC(const ChannelConfig *chancoeffs, ALsizei numchans, ALsizei numcoeffs, const ALfloat coeffs[MAX_AMBI_COEFFS], ALfloat ingain, ALfloat gains[MAX_OUTPUT_CHANNELS]);
void ComputePanningGainsBF(const BFChannelConfig *chanmap, ALsizei numchans, const ALfloat coeffs[MAX_AMBI_COEFFS], ALfloat ingain, ALfloat gains[MAX_OUTPUT_CHANNELS]);

/**
 * ComputeFirstOrderGains
 *
 * Sets channel gains for a first-order ambisonics input channel. The matrix is
 * a 1x4 'slice' of a transform matrix for the input channel, used to scale and
 * orient the sound samples.
 */
template<typename T>
void ComputeFirstOrderGains(
    const T& b,
    const ALfloat* const m,
    const ALfloat g,
    ALfloat* const o)
{
    if (b.coeff_count > 0)
    {
        ComputeFirstOrderGainsMC(b.ambi.coeffs, b.num_channels, m, g, o);
    }
    else
    {
        ComputeFirstOrderGainsBF(b.ambi.map, b.num_channels, m, g, o);
    }
}

void ComputeFirstOrderGainsMC(const ChannelConfig *chancoeffs, ALsizei numchans, const ALfloat mtx[4], ALfloat ingain, ALfloat gains[MAX_OUTPUT_CHANNELS]);
void ComputeFirstOrderGainsBF(const BFChannelConfig *chanmap, ALsizei numchans, const ALfloat mtx[4], ALfloat ingain, ALfloat gains[MAX_OUTPUT_CHANNELS]);


ALboolean MixSource(struct ALvoice *voice, struct ALsource *Source, ALCdevice *Device, ALsizei SamplesToDo);

void aluMixData(ALCdevice *device, ALvoid *OutBuffer, ALsizei NumSamples, const ALfloat* src_samples);
/* Caller must lock the device. */
void aluHandleDisconnect(ALCdevice *device);


#endif
