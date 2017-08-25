/**
 * OpenAL cross platform audio library
 * Copyright (C) 1999-2007 by authors.
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

#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <float.h>

#include "AL/al.h"
#include "AL/alc.h"
#include "alMain.h"
#include "alError.h"
#include "alSource.h"
#include "alBuffer.h"
#include "alThunk.h"
#include "alAuxEffectSlot.h"

#include "backends/base.h"

#include "almalloc.h"


extern inline struct ALsource *LookupSource(ALCcontext *context, ALuint id);
extern inline struct ALsource *RemoveSource(ALCcontext *context, ALuint id);

void InitSourceParams(ALsource *Source, ALsizei num_sends);
void DeinitSource(ALsource *source, ALsizei num_sends);
static void UpdateSourceProps(ALsource *source, ALvoice *voice, ALsizei num_sends);
static ALint64 GetSourceSampleOffset(ALsource *Source, ALCcontext *context, ALuint64 *clocktime);
static ALdouble GetSourceSecOffset(ALsource *Source, ALCcontext *context, ALuint64 *clocktime);
static ALdouble GetSourceOffset(ALsource *Source, ALenum name, ALCcontext *context);
static ALboolean GetSampleOffset(ALsource *Source, ALuint *offset, ALsizei *frac);
static ALboolean ApplyOffset(ALsource *Source, ALvoice *voice);

typedef enum SourceProp {
    srcPitch = AL_PITCH,
    srcGain = AL_GAIN,
    srcMinGain = AL_MIN_GAIN,
    srcMaxGain = AL_MAX_GAIN,
    srcMaxDistance = AL_MAX_DISTANCE,
    srcRolloffFactor = AL_ROLLOFF_FACTOR,
    srcDopplerFactor = AL_DOPPLER_FACTOR,
    srcConeOuterGain = AL_CONE_OUTER_GAIN,
    srcSecOffset = AL_SEC_OFFSET,
    srcSampleOffset = AL_SAMPLE_OFFSET,
    srcByteOffset = AL_BYTE_OFFSET,
    srcConeInnerAngle = AL_CONE_INNER_ANGLE,
    srcConeOuterAngle = AL_CONE_OUTER_ANGLE,
    srcRefDistance = AL_REFERENCE_DISTANCE,

    srcPosition = AL_POSITION,
    srcVelocity = AL_VELOCITY,
    srcDirection = AL_DIRECTION,

    srcSourceRelative = AL_SOURCE_RELATIVE,
    srcLooping = AL_LOOPING,
    srcBuffer = AL_BUFFER,
    srcSourceState = AL_SOURCE_STATE,
    srcBuffersQueued = AL_BUFFERS_QUEUED,
    srcBuffersProcessed = AL_BUFFERS_PROCESSED,
    srcSourceType = AL_SOURCE_TYPE,

    /* ALC_EXT_EFX */
    srcConeOuterGainHF = AL_CONE_OUTER_GAINHF,
    srcAirAbsorptionFactor = AL_AIR_ABSORPTION_FACTOR,
    srcRoomRolloffFactor =  AL_ROOM_ROLLOFF_FACTOR,
    srcDirectFilterGainHFAuto = AL_DIRECT_FILTER_GAINHF_AUTO,
    srcAuxSendFilterGainAuto = AL_AUXILIARY_SEND_FILTER_GAIN_AUTO,
    srcAuxSendFilterGainHFAuto = AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO,
    srcDirectFilter = AL_DIRECT_FILTER,
    srcAuxSendFilter = AL_AUXILIARY_SEND_FILTER,

    /* AL_SOFT_direct_channels */
    srcDirectChannelsSOFT = AL_DIRECT_CHANNELS_SOFT,

    /* AL_EXT_source_distance_model */
    srcDistanceModel = AL_DISTANCE_MODEL,

    srcByteLengthSOFT = AL_BYTE_LENGTH_SOFT,
    srcSampleLengthSOFT = AL_SAMPLE_LENGTH_SOFT,
    srcSecLengthSOFT = AL_SEC_LENGTH_SOFT,

    /* AL_SOFT_source_latency */
    srcSampleOffsetLatencySOFT = AL_SAMPLE_OFFSET_LATENCY_SOFT,
    srcSecOffsetLatencySOFT = AL_SEC_OFFSET_LATENCY_SOFT,

    /* AL_EXT_STEREO_ANGLES */
    srcAngles = AL_STEREO_ANGLES,

    /* AL_EXT_SOURCE_RADIUS */
    srcRadius = AL_SOURCE_RADIUS,

    /* AL_EXT_BFORMAT */
    srcOrientation = AL_ORIENTATION,

    /* AL_SOFT_source_resampler */
    srcResampler = AL_SOURCE_RESAMPLER_SOFT,

    /* AL_SOFT_source_spatialize */
    srcSpatialize = AL_SOURCE_SPATIALIZE_SOFT,
} SourceProp;

static ALboolean SetSourcefv(ALsource *Source, ALCcontext *Context, SourceProp prop, const ALfloat *values);
static ALboolean SetSourceiv(ALsource *Source, ALCcontext *Context, SourceProp prop, const ALint *values);
static ALboolean SetSourcei64v(ALsource *Source, ALCcontext *Context, SourceProp prop, const ALint64SOFT *values);

static ALboolean GetSourcedv(ALsource *Source, ALCcontext *Context, SourceProp prop, ALdouble *values);
static ALboolean GetSourceiv(ALsource *Source, ALCcontext *Context, SourceProp prop, ALint *values);
static ALboolean GetSourcei64v(ALsource *Source, ALCcontext *Context, SourceProp prop, ALint64 *values);

static inline ALvoice *GetSourceVoice(const ALsource *source, const ALCcontext *context)
{
    ALvoice **voice = context->Voices;
    ALvoice **voice_end = voice + context->VoiceCount;
    while(voice != voice_end)
    {
        if((*voice)->Source == source)
            return *voice;
        ++voice;
    }
    return NULL;
}

/**
 * Returns if the last known state for the source was playing or paused. Does
 * not sync with the mixer voice.
 */
static inline bool IsPlayingOrPaused(ALsource *source)
{
    ALenum state = source->state;
    return state == AL_PLAYING || state == AL_PAUSED;
}

/**
 * Returns an updated source state using the matching voice's status (or lack
 * thereof).
 */
static inline ALenum GetSourceState(ALsource *source, ALvoice *voice)
{
    if(!voice)
    {
        ALenum state = AL_PLAYING;
        if(source->state == state ? (source->state = AL_STOPPED, true) : (state = source->state, false))
            return AL_STOPPED;
        return state; 
    }
    return source->state;
}

/**
 * Returns if the source should specify an update, given the context's
 * deferring state and the source's last known state.
 */
static inline bool SourceShouldUpdate(ALsource *source, ALCcontext *context)
{
    return !context->DeferUpdates &&
           IsPlayingOrPaused(source);
}

static ALint FloatValsByProp(ALenum prop)
{
    if(prop != (ALenum)((SourceProp)prop))
        return 0;
    switch((SourceProp)prop)
    {
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_MAX_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_DOPPLER_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_REFERENCE_DISTANCE:
        case AL_CONE_OUTER_GAINHF:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
        case AL_SOURCE_RADIUS:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            return 1;

        case AL_STEREO_ANGLES:
            return 2;

        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            return 3;

        case AL_ORIENTATION:
            return 6;

        case AL_SEC_OFFSET_LATENCY_SOFT:
            break; /* Double only */

        case AL_BUFFER:
        case AL_DIRECT_FILTER:
        case AL_AUXILIARY_SEND_FILTER:
            break; /* i/i64 only */
        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            break; /* i64 only */
    }
    return 0;
}
static ALint DoubleValsByProp(ALenum prop)
{
    if(prop != (ALenum)((SourceProp)prop))
        return 0;
    switch((SourceProp)prop)
    {
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_MAX_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_DOPPLER_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_REFERENCE_DISTANCE:
        case AL_CONE_OUTER_GAINHF:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
        case AL_SOURCE_RADIUS:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            return 1;

        case AL_SEC_OFFSET_LATENCY_SOFT:
        case AL_STEREO_ANGLES:
            return 2;

        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            return 3;

        case AL_ORIENTATION:
            return 6;

        case AL_BUFFER:
        case AL_DIRECT_FILTER:
        case AL_AUXILIARY_SEND_FILTER:
            break; /* i/i64 only */
        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            break; /* i64 only */
    }
    return 0;
}

static ALint IntValsByProp(ALenum prop)
{
    if(prop != (ALenum)((SourceProp)prop))
        return 0;
    switch((SourceProp)prop)
    {
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_MAX_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_DOPPLER_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_REFERENCE_DISTANCE:
        case AL_CONE_OUTER_GAINHF:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_BUFFER:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE:
        case AL_DIRECT_FILTER:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
        case AL_SOURCE_RADIUS:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            return 1;

        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
        case AL_AUXILIARY_SEND_FILTER:
            return 3;

        case AL_ORIENTATION:
            return 6;

        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            break; /* i64 only */
        case AL_SEC_OFFSET_LATENCY_SOFT:
            break; /* Double only */
        case AL_STEREO_ANGLES:
            break; /* Float/double only */
    }
    return 0;
}
static ALint Int64ValsByProp(ALenum prop)
{
    if(prop != (ALenum)((SourceProp)prop))
        return 0;
    switch((SourceProp)prop)
    {
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_MAX_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_DOPPLER_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_REFERENCE_DISTANCE:
        case AL_CONE_OUTER_GAINHF:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_BUFFER:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE:
        case AL_DIRECT_FILTER:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
        case AL_SOURCE_RADIUS:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            return 1;

        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            return 2;

        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
        case AL_AUXILIARY_SEND_FILTER:
            return 3;

        case AL_ORIENTATION:
            return 6;

        case AL_SEC_OFFSET_LATENCY_SOFT:
            break; /* Double only */
        case AL_STEREO_ANGLES:
            break; /* Float/double only */
    }
    return 0;
}


#define CHECKVAL(x) do {                                                      \
    if(!(x))                                                                  \
        SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_VALUE, AL_FALSE);      \
} while(0)

#define DO_UPDATEPROPS() do {                                                 \
    ALvoice *voice;                                                           \
    if(SourceShouldUpdate(Source, Context) &&                                 \
       (voice=GetSourceVoice(Source, Context)) != NULL)                       \
        UpdateSourceProps(Source, voice, device->NumAuxSends);                \
    else                                                                      \
        Source->PropsClean = 0;       \
} while(0)

static ALboolean SetSourcefv(ALsource *Source, ALCcontext *Context, SourceProp prop, const ALfloat *values)
{
    ALCdevice *device = Context->Device;
    ALint ival;

    switch(prop)
    {
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
        case AL_SEC_OFFSET_LATENCY_SOFT:
            /* Query only */
            SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_OPERATION, AL_FALSE);

        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            CHECKVAL(*values >= 0.0f);

            Source->OffsetType = prop;
            Source->Offset = *values;

            if(IsPlayingOrPaused(Source))
            {
                ALvoice *voice;

                /* Double-check that the source is still playing while we have
                 * the lock.
                 */
                voice = GetSourceVoice(Source, Context);
                if(voice)
                {
                    if(ApplyOffset(Source, voice) == AL_FALSE)
                    {
                        SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_VALUE, AL_FALSE);
                    }
                }
            }
            return AL_TRUE;

        case AL_SOURCE_RADIUS:
            CHECKVAL(*values >= 0.0f && isfinite(*values));

            Source->Radius = *values;
            DO_UPDATEPROPS();
            return AL_TRUE;

        case AL_STEREO_ANGLES:
            CHECKVAL(isfinite(values[0]) && isfinite(values[1]));

            Source->StereoPan[0] = values[0];
            Source->StereoPan[1] = values[1];
            DO_UPDATEPROPS();
            return AL_TRUE;


        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_SOURCE_STATE:
        case AL_SOURCE_TYPE:
        case AL_DISTANCE_MODEL:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            ival = (ALint)values[0];
            return SetSourceiv(Source, Context, prop, &ival);

        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
            ival = (ALint)((ALuint)values[0]);
            return SetSourceiv(Source, Context, prop, &ival);

        case AL_BUFFER:
        case AL_DIRECT_FILTER:
        case AL_AUXILIARY_SEND_FILTER:
        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            break;
    }

    ERR("Unexpected property: 0x%04x\n", prop);
    SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_ENUM, AL_FALSE);
}

static ALboolean SetSourceiv(ALsource *Source, ALCcontext *Context, SourceProp prop, const ALint *values)
{
    ALCdevice *device = Context->Device;
    ALfilter  *filter = NULL;
    ALeffectslot *slot = NULL;
    ALfloat fvals[6];

    switch(prop)
    {
        case AL_SOURCE_STATE:
        case AL_SOURCE_TYPE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
            /* Query only */
            SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_OPERATION, AL_FALSE);

        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            CHECKVAL(*values >= 0);

            Source->OffsetType = prop;
            Source->Offset = *values;

            if(IsPlayingOrPaused(Source))
            {
                ALvoice *voice;

                voice = GetSourceVoice(Source, Context);
                if(voice)
                {
                    if(ApplyOffset(Source, voice) == AL_FALSE)
                    {
                        SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_VALUE, AL_FALSE);
                    }
                }
            }
            return AL_TRUE;

        case AL_DIRECT_FILTER:
            if(!(*values == 0 || (filter=LookupFilter(device, *values)) != NULL))
            {
                SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_VALUE, AL_FALSE);
            }

            if(!filter)
            {
                Source->Direct.Gain = 1.0f;
                Source->Direct.GainHF = 1.0f;
                Source->Direct.HFReference = LOWPASSFREQREF;
                Source->Direct.GainLF = 1.0f;
                Source->Direct.LFReference = HIGHPASSFREQREF;
            }
            else
            {
                Source->Direct.Gain = filter->Gain;
                Source->Direct.GainHF = filter->GainHF;
                Source->Direct.HFReference = filter->HFReference;
                Source->Direct.GainLF = filter->GainLF;
                Source->Direct.LFReference = filter->LFReference;
            }
            DO_UPDATEPROPS();
            return AL_TRUE;

        case AL_AUXILIARY_SEND_FILTER:
            slot = Context->Device->effect_slot;

            if(!((ALuint)values[1] < (ALuint)device->NumAuxSends &&
                 (values[0] == 0 || !slot) &&
                 (values[2] == 0 || (filter=LookupFilter(device, values[2])) != NULL)))
            {
                SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_VALUE, AL_FALSE);
            }

            if(!filter)
            {
                /* Disable filter */
                Source->Send[values[1]].Gain = 1.0f;
                Source->Send[values[1]].GainHF = 1.0f;
                Source->Send[values[1]].HFReference = LOWPASSFREQREF;
                Source->Send[values[1]].GainLF = 1.0f;
                Source->Send[values[1]].LFReference = HIGHPASSFREQREF;
            }
            else
            {
                Source->Send[values[1]].Gain = filter->Gain;
                Source->Send[values[1]].GainHF = filter->GainHF;
                Source->Send[values[1]].HFReference = filter->HFReference;
                Source->Send[values[1]].GainLF = filter->GainLF;
                Source->Send[values[1]].LFReference = filter->LFReference;
            }

            if(slot != Source->Send[values[1]].Slot && IsPlayingOrPaused(Source))
            {
                ALvoice *voice;
                /* Add refcount on the new slot, and release the previous slot */
                if(slot) slot->ref += 1;
                if(Source->Send[values[1]].Slot)
                    Source->Send[values[1]].Slot->ref -= 1;
                Source->Send[values[1]].Slot = slot;

                /* We must force an update if the auxiliary slot changed on an
                 * active source, in case the slot is about to be deleted.
                 */
                if((voice=GetSourceVoice(Source, Context)) != NULL)
                    UpdateSourceProps(Source, voice, device->NumAuxSends);
                else
                    Source->PropsClean = 0;
            }
            else
            {
                if(slot) slot->ref += 1;
                if(Source->Send[values[1]].Slot)
                    Source->Send[values[1]].Slot->ref -= 1;
                Source->Send[values[1]].Slot = slot;
                DO_UPDATEPROPS();
            }

            return AL_TRUE;


        /* 1x float */
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_MAX_DISTANCE:
        case AL_DOPPLER_FACTOR:
        case AL_CONE_OUTER_GAINHF:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_SOURCE_RADIUS:
            fvals[0] = (ALfloat)*values;
            return SetSourcefv(Source, Context, (int)prop, fvals);

        /* 3x float */
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            fvals[0] = (ALfloat)values[0];
            fvals[1] = (ALfloat)values[1];
            fvals[2] = (ALfloat)values[2];
            return SetSourcefv(Source, Context, (int)prop, fvals);

        /* 6x float */
        case AL_ORIENTATION:
            fvals[0] = (ALfloat)values[0];
            fvals[1] = (ALfloat)values[1];
            fvals[2] = (ALfloat)values[2];
            fvals[3] = (ALfloat)values[3];
            fvals[4] = (ALfloat)values[4];
            fvals[5] = (ALfloat)values[5];
            return SetSourcefv(Source, Context, (int)prop, fvals);

        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
        case AL_SEC_OFFSET_LATENCY_SOFT:
        case AL_STEREO_ANGLES:
            break;
    }

    ERR("Unexpected property: 0x%04x\n", prop);
    SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_ENUM, AL_FALSE);
}

static ALboolean SetSourcei64v(ALsource *Source, ALCcontext *Context, SourceProp prop, const ALint64SOFT *values)
{
    ALfloat fvals[6];
    ALint   ivals[3];

    switch(prop)
    {
        case AL_SOURCE_TYPE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_STATE:
        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SEC_LENGTH_SOFT:
            /* Query only */
            SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_OPERATION, AL_FALSE);


        /* 1x int */
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            CHECKVAL(*values <= INT_MAX && *values >= INT_MIN);

            ivals[0] = (ALint)*values;
            return SetSourceiv(Source, Context, (int)prop, ivals);

        /* 1x uint */
        case AL_BUFFER:
        case AL_DIRECT_FILTER:
            CHECKVAL(*values <= UINT_MAX && *values >= 0);

            ivals[0] = (ALuint)*values;
            return SetSourceiv(Source, Context, (int)prop, ivals);

        /* 3x uint */
        case AL_AUXILIARY_SEND_FILTER:
            CHECKVAL(values[0] <= UINT_MAX && values[0] >= 0 &&
                     values[1] <= UINT_MAX && values[1] >= 0 &&
                     values[2] <= UINT_MAX && values[2] >= 0);

            ivals[0] = (ALuint)values[0];
            ivals[1] = (ALuint)values[1];
            ivals[2] = (ALuint)values[2];
            return SetSourceiv(Source, Context, (int)prop, ivals);

        /* 1x float */
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_MAX_DISTANCE:
        case AL_DOPPLER_FACTOR:
        case AL_CONE_OUTER_GAINHF:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_SOURCE_RADIUS:
            fvals[0] = (ALfloat)*values;
            return SetSourcefv(Source, Context, (int)prop, fvals);

        /* 3x float */
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            fvals[0] = (ALfloat)values[0];
            fvals[1] = (ALfloat)values[1];
            fvals[2] = (ALfloat)values[2];
            return SetSourcefv(Source, Context, (int)prop, fvals);

        /* 6x float */
        case AL_ORIENTATION:
            fvals[0] = (ALfloat)values[0];
            fvals[1] = (ALfloat)values[1];
            fvals[2] = (ALfloat)values[2];
            fvals[3] = (ALfloat)values[3];
            fvals[4] = (ALfloat)values[4];
            fvals[5] = (ALfloat)values[5];
            return SetSourcefv(Source, Context, (int)prop, fvals);

        case AL_SEC_OFFSET_LATENCY_SOFT:
        case AL_STEREO_ANGLES:
            break;
    }

    ERR("Unexpected property: 0x%04x\n", prop);
    SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_ENUM, AL_FALSE);
}

#undef CHECKVAL


static ALboolean GetSourcedv(ALsource *Source, ALCcontext *Context, SourceProp prop, ALdouble *values)
{
    ALCdevice *device = Context->Device;
    ALint ivals[3];
    ALboolean err;

    switch(prop)
    {
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
            *values = GetSourceOffset(Source, prop, Context);
            return AL_TRUE;

        case AL_SOURCE_RADIUS:
            *values = Source->Radius;
            return AL_TRUE;

        case AL_STEREO_ANGLES:
            values[0] = Source->StereoPan[0];
            values[1] = Source->StereoPan[1];
            return AL_TRUE;

        case AL_SEC_OFFSET_LATENCY_SOFT:
            return AL_FALSE;

        /* 1x int */
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_SOURCE_TYPE:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            if((err=GetSourceiv(Source, Context, (int)prop, ivals)) != AL_FALSE)
                *values = (ALdouble)ivals[0];
            return err;

        case AL_BUFFER:
        case AL_DIRECT_FILTER:
        case AL_AUXILIARY_SEND_FILTER:
        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            break;
    }

    ERR("Unexpected property: 0x%04x\n", prop);
    SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_ENUM, AL_FALSE);
}

static ALboolean GetSourceiv(ALsource *Source, ALCcontext *Context, SourceProp prop, ALint *values)
{
    ALdouble dvals[6];
    ALboolean err;

    switch(prop)
    {
        case AL_SOURCE_STATE:
            *values = GetSourceState(Source, GetSourceVoice(Source, Context));
            return AL_TRUE;

        case AL_SOURCE_TYPE:
            *values = Source->SourceType;
            return AL_TRUE;

        /* 1x float/double */
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_MAX_DISTANCE:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_DOPPLER_FACTOR:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_CONE_OUTER_GAINHF:
        case AL_SEC_LENGTH_SOFT:
        case AL_SOURCE_RADIUS:
            if((err=GetSourcedv(Source, Context, prop, dvals)) != AL_FALSE)
                *values = (ALint)dvals[0];
            return err;

        /* 3x float/double */
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            if((err=GetSourcedv(Source, Context, prop, dvals)) != AL_FALSE)
            {
                values[0] = (ALint)dvals[0];
                values[1] = (ALint)dvals[1];
                values[2] = (ALint)dvals[2];
            }
            return err;

        /* 6x float/double */
        case AL_ORIENTATION:
            if((err=GetSourcedv(Source, Context, prop, dvals)) != AL_FALSE)
            {
                values[0] = (ALint)dvals[0];
                values[1] = (ALint)dvals[1];
                values[2] = (ALint)dvals[2];
                values[3] = (ALint)dvals[3];
                values[4] = (ALint)dvals[4];
                values[5] = (ALint)dvals[5];
            }
            return err;

        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            break; /* i64 only */
        case AL_SEC_OFFSET_LATENCY_SOFT:
            break; /* Double only */
        case AL_STEREO_ANGLES:
            break; /* Float/double only */

        case AL_DIRECT_FILTER:
        case AL_AUXILIARY_SEND_FILTER:
            break; /* ??? */
    }

    ERR("Unexpected property: 0x%04x\n", prop);
    SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_ENUM, AL_FALSE);
}

static ALboolean GetSourcei64v(ALsource *Source, ALCcontext *Context, SourceProp prop, ALint64 *values)
{
    ALCdevice *device = Context->Device;
    ALdouble dvals[6];
    ALint ivals[3];
    ALboolean err;

    switch(prop)
    {
        case AL_SAMPLE_OFFSET_LATENCY_SOFT:
            return AL_FALSE;

        /* 1x float/double */
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_CONE_OUTER_GAIN:
        case AL_MAX_DISTANCE:
        case AL_SEC_OFFSET:
        case AL_SAMPLE_OFFSET:
        case AL_BYTE_OFFSET:
        case AL_DOPPLER_FACTOR:
        case AL_AIR_ABSORPTION_FACTOR:
        case AL_ROOM_ROLLOFF_FACTOR:
        case AL_CONE_OUTER_GAINHF:
        case AL_SEC_LENGTH_SOFT:
        case AL_SOURCE_RADIUS:
            if((err=GetSourcedv(Source, Context, prop, dvals)) != AL_FALSE)
                *values = (ALint64)dvals[0];
            return err;

        /* 3x float/double */
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            if((err=GetSourcedv(Source, Context, prop, dvals)) != AL_FALSE)
            {
                values[0] = (ALint64)dvals[0];
                values[1] = (ALint64)dvals[1];
                values[2] = (ALint64)dvals[2];
            }
            return err;

        /* 6x float/double */
        case AL_ORIENTATION:
            if((err=GetSourcedv(Source, Context, prop, dvals)) != AL_FALSE)
            {
                values[0] = (ALint64)dvals[0];
                values[1] = (ALint64)dvals[1];
                values[2] = (ALint64)dvals[2];
                values[3] = (ALint64)dvals[3];
                values[4] = (ALint64)dvals[4];
                values[5] = (ALint64)dvals[5];
            }
            return err;

        /* 1x int */
        case AL_SOURCE_RELATIVE:
        case AL_LOOPING:
        case AL_SOURCE_STATE:
        case AL_BUFFERS_QUEUED:
        case AL_BUFFERS_PROCESSED:
        case AL_BYTE_LENGTH_SOFT:
        case AL_SAMPLE_LENGTH_SOFT:
        case AL_SOURCE_TYPE:
        case AL_DIRECT_FILTER_GAINHF_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
        case AL_DIRECT_CHANNELS_SOFT:
        case AL_DISTANCE_MODEL:
        case AL_SOURCE_RESAMPLER_SOFT:
        case AL_SOURCE_SPATIALIZE_SOFT:
            if((err=GetSourceiv(Source, Context, prop, ivals)) != AL_FALSE)
                *values = ivals[0];
            return err;

        /* 1x uint */
        case AL_BUFFER:
        case AL_DIRECT_FILTER:
            if((err=GetSourceiv(Source, Context, prop, ivals)) != AL_FALSE)
                *values = (ALuint)ivals[0];
            return err;

        /* 3x uint */
        case AL_AUXILIARY_SEND_FILTER:
            if((err=GetSourceiv(Source, Context, prop, ivals)) != AL_FALSE)
            {
                values[0] = (ALuint)ivals[0];
                values[1] = (ALuint)ivals[1];
                values[2] = (ALuint)ivals[2];
            }
            return err;

        case AL_SEC_OFFSET_LATENCY_SOFT:
            break; /* Double only */
        case AL_STEREO_ANGLES:
            break; /* Float/double only */
    }

    ERR("Unexpected property: 0x%04x\n", prop);
    SET_ERROR_AND_RETURN_VALUE(Context, AL_INVALID_ENUM, AL_FALSE);
}


AL_API ALvoid AL_APIENTRY alGenSources(ALsizei n, ALuint *sources)
{
}


AL_API ALvoid AL_APIENTRY alDeleteSources(ALsizei n, const ALuint *sources)
{
}


AL_API ALboolean AL_APIENTRY alIsSource(ALuint source)
{
    ALCcontext *context;
    ALboolean ret;

    context = GetContextRef();
    if(!context) return AL_FALSE;

    ret = (LookupSource(context, source) ? AL_TRUE : AL_FALSE);

    ALCcontext_DecRef(context);

    return ret;
}


AL_API ALvoid AL_APIENTRY alSourcef(ALuint source, ALenum param, ALfloat value)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(FloatValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
        SetSourcefv(Source, Context, param, &value);

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alSource3f(ALuint source, ALenum param, ALfloat value1, ALfloat value2, ALfloat value3)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    Source=Context->Device->source;
    if(!(FloatValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALfloat fvals[3] = { value1, value2, value3 };
        SetSourcefv(Source, Context, param, fvals);
    }

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alSourcefv(ALuint source, ALenum param, const ALfloat *values)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(FloatValsByProp(param) > 0))
        alSetError(Context, AL_INVALID_ENUM);
    else
        SetSourcefv(Source, Context, param, values);

    ALCcontext_DecRef(Context);
}


AL_API ALvoid AL_APIENTRY alSourcedSOFT(ALuint source, ALenum param, ALdouble value)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(DoubleValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALfloat fval = (ALfloat)value;
        SetSourcefv(Source, Context, param, &fval);
    }

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alSource3dSOFT(ALuint source, ALenum param, ALdouble value1, ALdouble value2, ALdouble value3)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(DoubleValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALfloat fvals[3] = { (ALfloat)value1, (ALfloat)value2, (ALfloat)value3 };
        SetSourcefv(Source, Context, param, fvals);
    }

    ALCcontext_DecRef(Context);
}

AL_API ALvoid AL_APIENTRY alSourcedvSOFT(ALuint source, ALenum param, const ALdouble *values)
{
    ALCcontext *Context;
    ALsource   *Source;
    ALint      count;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!((count=DoubleValsByProp(param)) > 0 && count <= 6))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALfloat fvals[6];
        ALint i;

        for(i = 0;i < count;i++)
            fvals[i] = (ALfloat)values[i];
        SetSourcefv(Source, Context, param, fvals);
    }

    ALCcontext_DecRef(Context);
}


AL_API ALvoid AL_APIENTRY alSourcei(ALuint source, ALenum param, ALint value)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    Source = Context->Device->source;
    if(!(IntValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
        SetSourceiv(Source, Context, param, &value);

    ALCcontext_DecRef(Context);
}

AL_API void AL_APIENTRY alSource3i(ALuint source, ALenum param, ALint value1, ALint value2, ALint value3)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    Source=Context->Device->source;
    if(!(IntValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALint ivals[3] = { value1, value2, value3 };
        SetSourceiv(Source, Context, param, ivals);
    }

    ALCcontext_DecRef(Context);
}

AL_API void AL_APIENTRY alSourceiv(ALuint source, ALenum param, const ALint *values)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(IntValsByProp(param) > 0))
        alSetError(Context, AL_INVALID_ENUM);
    else
        SetSourceiv(Source, Context, param, values);

    ALCcontext_DecRef(Context);
}


AL_API ALvoid AL_APIENTRY alSourcei64SOFT(ALuint source, ALenum param, ALint64SOFT value)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(Int64ValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
        SetSourcei64v(Source, Context, param, &value);

    ALCcontext_DecRef(Context);
}

AL_API void AL_APIENTRY alSource3i64SOFT(ALuint source, ALenum param, ALint64SOFT value1, ALint64SOFT value2, ALint64SOFT value3)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(Int64ValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALint64SOFT i64vals[3] = { value1, value2, value3 };
        SetSourcei64v(Source, Context, param, i64vals);
    }

    ALCcontext_DecRef(Context);
}

AL_API void AL_APIENTRY alSourcei64vSOFT(ALuint source, ALenum param, const ALint64SOFT *values)
{
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(Int64ValsByProp(param) > 0))
        alSetError(Context, AL_INVALID_ENUM);
    else
        SetSourcei64v(Source, Context, param, values);

    ALCcontext_DecRef(Context);
}


AL_API ALvoid AL_APIENTRY alGetSourcef(ALuint source, ALenum param, ALfloat *value)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!value)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(FloatValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALdouble dval;
        if(GetSourcedv(Source, Context, param, &dval))
            *value = (ALfloat)dval;
    }

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API ALvoid AL_APIENTRY alGetSource3f(ALuint source, ALenum param, ALfloat *value1, ALfloat *value2, ALfloat *value3)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(value1 && value2 && value3))
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(FloatValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALdouble dvals[3];
        if(GetSourcedv(Source, Context, param, dvals))
        {
            *value1 = (ALfloat)dvals[0];
            *value2 = (ALfloat)dvals[1];
            *value3 = (ALfloat)dvals[2];
        }
    }

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API ALvoid AL_APIENTRY alGetSourcefv(ALuint source, ALenum param, ALfloat *values)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;
    ALint      count;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!((count=FloatValsByProp(param)) > 0 && count <= 6))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALdouble dvals[6];
        if(GetSourcedv(Source, Context, param, dvals))
        {
            ALint i;
            for(i = 0;i < count;i++)
                values[i] = (ALfloat)dvals[i];
        }
    }

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API void AL_APIENTRY alGetSourcedSOFT(ALuint source, ALenum param, ALdouble *value)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!value)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(DoubleValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
        GetSourcedv(Source, Context, param, value);

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}

AL_API void AL_APIENTRY alGetSource3dSOFT(ALuint source, ALenum param, ALdouble *value1, ALdouble *value2, ALdouble *value3)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(value1 && value2 && value3))
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(DoubleValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALdouble dvals[3];
        if(GetSourcedv(Source, Context, param, dvals))
        {
            *value1 = dvals[0];
            *value2 = dvals[1];
            *value3 = dvals[2];
        }
    }

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}

AL_API void AL_APIENTRY alGetSourcedvSOFT(ALuint source, ALenum param, ALdouble *values)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(DoubleValsByProp(param) > 0))
        alSetError(Context, AL_INVALID_ENUM);
    else
        GetSourcedv(Source, Context, param, values);

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API ALvoid AL_APIENTRY alGetSourcei(ALuint source, ALenum param, ALint *value)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!value)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(IntValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
        GetSourceiv(Source, Context, param, value);

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API void AL_APIENTRY alGetSource3i(ALuint source, ALenum param, ALint *value1, ALint *value2, ALint *value3)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(value1 && value2 && value3))
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(IntValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALint ivals[3];
        if(GetSourceiv(Source, Context, param, ivals))
        {
            *value1 = ivals[0];
            *value2 = ivals[1];
            *value3 = ivals[2];
        }
    }

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API void AL_APIENTRY alGetSourceiv(ALuint source, ALenum param, ALint *values)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(IntValsByProp(param) > 0))
        alSetError(Context, AL_INVALID_ENUM);
    else
        GetSourceiv(Source, Context, param, values);

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API void AL_APIENTRY alGetSourcei64SOFT(ALuint source, ALenum param, ALint64SOFT *value)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!value)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(Int64ValsByProp(param) == 1))
        alSetError(Context, AL_INVALID_ENUM);
    else
        GetSourcei64v(Source, Context, param, value);

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}

AL_API void AL_APIENTRY alGetSource3i64SOFT(ALuint source, ALenum param, ALint64SOFT *value1, ALint64SOFT *value2, ALint64SOFT *value3)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!(value1 && value2 && value3))
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(Int64ValsByProp(param) == 3))
        alSetError(Context, AL_INVALID_ENUM);
    else
    {
        ALint64 i64vals[3];
        if(GetSourcei64v(Source, Context, param, i64vals))
        {
            *value1 = i64vals[0];
            *value2 = i64vals[1];
            *value3 = i64vals[2];
        }
    }

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}

AL_API void AL_APIENTRY alGetSourcei64vSOFT(ALuint source, ALenum param, ALint64SOFT *values)
{
#if 0
    ALCcontext *Context;
    ALsource   *Source;

    Context = GetContextRef();
    if(!Context) return;

    if((Source=LookupSource(Context, source)) == NULL)
        alSetError(Context, AL_INVALID_NAME);
    else if(!values)
        alSetError(Context, AL_INVALID_VALUE);
    else if(!(Int64ValsByProp(param) > 0))
        alSetError(Context, AL_INVALID_ENUM);
    else
        GetSourcei64v(Source, Context, param, values);

    ALCcontext_DecRef(Context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API ALvoid AL_APIENTRY alSourcePlay(ALuint source)
{
    alSourcePlayv(1, &source);
}
AL_API ALvoid AL_APIENTRY alSourcePlayv(ALsizei n, const ALuint *sources)
{
    ALCcontext *context;
    ALCdevice *device;
    ALsource *source;
    ALvoice *voice;
    ALsizei i, j;

    context = GetContextRef();
    if(!context) return;

    if(!(n == 1))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);

    device = context->Device;

    for(i = 0;i < n;i++)
    {
        bool start_fading = false;
        ALsizei s;

        source = context->Device->source;

        voice = GetSourceVoice(source, context);
        switch(GetSourceState(source, voice))
        {
            case AL_PLAYING:
                assert(voice != NULL);
                /* A source that's already playing is restarted from the beginning. */
                voice->position = 0;
                voice->position_fraction = 0;
                goto finish_play;

            case AL_PAUSED:
                assert(voice != NULL);
                /* A source that's paused simply resumes. */
                voice->Playing = true;
                source->state = AL_PLAYING;
                goto finish_play;

            default:
                break;
        }

        /* Make sure this source isn't already active, and if not, look for an
         * unused voice to put it in.
         */
        assert(voice == NULL);
        for(j = 0;j < context->VoiceCount;j++)
        {
            if(context->Voices[j]->Source == NULL)
            {
                voice = context->Voices[j];
                break;
            }
        }
        if(voice == NULL)
            voice = context->Voices[context->VoiceCount++];
        voice->Playing = false;

        source->PropsClean = 1;
        UpdateSourceProps(source, voice, device->NumAuxSends);

        voice->position = 0;
        voice->position_fraction = 0;
        if(source->OffsetType != AL_NONE)
        {
            ApplyOffset(source, voice);
            start_fading = voice->position != 0 ||
                voice->position_fraction != 0;
        }

        voice->NumChannels = device->Dry.NumChannels;

        /* Clear the stepping value so the mixer knows not to mix this until
         * the update gets applied.
         */
        voice->Step = 0;

        voice->Flags = start_fading ? VOICE_IS_FADING : 0;
        memset(voice->Direct.Params, 0, sizeof(voice->Direct.Params[0])*voice->NumChannels);
        for(s = 0;s < device->NumAuxSends;s++)
            memset(voice->Send[s].Params, 0, sizeof(voice->Send[s].Params[0])*voice->NumChannels);

        voice->Source = source;
        voice->Playing = true;
        source->state = AL_PLAYING;
    finish_play:
        ;
    }

done:
    ALCcontext_DecRef(context);
}

AL_API ALvoid AL_APIENTRY alSourcePause(ALuint source)
{
    alSourcePausev(1, &source);
}
AL_API ALvoid AL_APIENTRY alSourcePausev(ALsizei n, const ALuint *sources)
{
#if 0
    ALCcontext *context;
    ALCdevice *device;
    ALsource *source;
    ALvoice *voice;
    ALsizei i;

    context = GetContextRef();
    if(!context) return;

    if(!(n >= 0))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    for(i = 0;i < n;i++)
    {
        if(!LookupSource(context, sources[i]))
            SET_ERROR_AND_GOTO(context, AL_INVALID_NAME, done);
    }

    device = context->Device;
    for(i = 0;i < n;i++)
    {
        source = LookupSource(context, sources[i]);
        if((voice=GetSourceVoice(source, context)) != NULL)
        {
            voice->Playing = false;
        }
        if(GetSourceState(source, voice) == AL_PLAYING)
            source->state = AL_PAUSED;
    }

done:
    ALCcontext_DecRef(context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}

AL_API ALvoid AL_APIENTRY alSourceStop(ALuint source)
{
    alSourceStopv(1, &source);
}
AL_API ALvoid AL_APIENTRY alSourceStopv(ALsizei n, const ALuint *sources)
{
    ALCcontext *context;
    ALCdevice *device;
    ALsource *source;
    ALvoice *voice;
    ALsizei i;

    context = GetContextRef();
    if(!context) return;

    if(!(n == 1))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);

    device = context->Device;
    for(i = 0;i < n;i++)
    {
        source = context->Device->source;
        if((voice=GetSourceVoice(source, context)) != NULL)
        {
            voice->Source = NULL;
            voice->Playing = false;
        }
        if(source->state != AL_INITIAL)
            source->state = AL_STOPPED;
        source->OffsetType = AL_NONE;
        source->Offset = 0.0;
    }

done:
    ALCcontext_DecRef(context);
}

AL_API ALvoid AL_APIENTRY alSourceRewind(ALuint source)
{
    alSourceRewindv(1, &source);
}
AL_API ALvoid AL_APIENTRY alSourceRewindv(ALsizei n, const ALuint *sources)
{
#if 0
    ALCcontext *context;
    ALCdevice *device;
    ALsource *source;
    ALvoice *voice;
    ALsizei i;

    context = GetContextRef();
    if(!context) return;

    if(!(n >= 0))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    for(i = 0;i < n;i++)
    {
        if(!LookupSource(context, sources[i]))
            SET_ERROR_AND_GOTO(context, AL_INVALID_NAME, done);
    }

    device = context->Device;
    for(i = 0;i < n;i++)
    {
        source = LookupSource(context, sources[i]);
        if((voice=GetSourceVoice(source, context)) != NULL)
        {
            voice->Source = NULL;
            voice->Playing = false;
        }
        if(source->state != AL_INITIAL)
            source->state = AL_INITIAL;
        source->OffsetType = AL_NONE;
        source->Offset = 0.0;
    }

done:
    ALCcontext_DecRef(context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


AL_API ALvoid AL_APIENTRY alSourceQueueBuffers(ALuint src, ALsizei nb, const ALuint *buffers)
{
#if 0
    ALCdevice *device;
    ALCcontext *context;
    ALsource *source;
    ALsizei i;
    ALbufferlistitem *BufferListStart;
    ALbufferlistitem *BufferList;
    ALbuffer *BufferFmt = NULL;

    if(nb == 0)
        return;

    context = GetContextRef();
    if(!context) return;

    device = context->Device;

    if(!(nb >= 0))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    if((source=LookupSource(context, src)) == NULL)
        SET_ERROR_AND_GOTO(context, AL_INVALID_NAME, done);

    if(source->SourceType == AL_STATIC)
    {
        /* Can't queue on a Static Source */
        SET_ERROR_AND_GOTO(context, AL_INVALID_OPERATION, done);
    }

    /* Check for a valid Buffer, for its frequency and format */
    BufferList = source->queue;
    while(BufferList)
    {
        if(BufferList->buffer)
        {
            BufferFmt = BufferList->buffer;
            break;
        }
        BufferList = BufferList->next;
    }

    BufferListStart = NULL;
    BufferList = NULL;
    for(i = 0;i < nb;i++)
    {
        ALbuffer *buffer = NULL;
        if(buffers[i] && (buffer=LookupBuffer(device, buffers[i])) == NULL)
        {
            SET_ERROR_AND_GOTO(context, AL_INVALID_NAME, buffer_error);
        }

        if(!BufferListStart)
        {
            BufferListStart = al_calloc(DEF_ALIGN, sizeof(ALbufferlistitem));
            BufferList = BufferListStart;
        }
        else
        {
            ALbufferlistitem *item = al_calloc(DEF_ALIGN, sizeof(ALbufferlistitem));
            BufferList->next = item;
            BufferList = item;
        }
        BufferList->buffer = buffer;
        BufferList->next = NULL;
        if(!buffer) continue;

        /* Hold a read lock on each buffer being queued while checking all
         * provided buffers. This is done so other threads don't see an extra
         * reference on some buffers if this operation ends up failing. */
        buffer->ref += 1;

        if(BufferFmt == NULL)
            BufferFmt = buffer;
        else if(BufferFmt->Frequency != buffer->Frequency ||
                BufferFmt->OriginalChannels != buffer->OriginalChannels ||
                BufferFmt->OriginalType != buffer->OriginalType)
        {
            SET_ERROR_AND_GOTO(context, AL_INVALID_OPERATION, buffer_error);

        buffer_error:
            /* A buffer failed (invalid ID or format), so unlock and release
             * each buffer we had. */
            while(BufferListStart)
            {
                ALbufferlistitem *next = BufferListStart->next;
                if((buffer=BufferListStart->buffer) != NULL)
                {
                    buffer->ref -= 1;
                }
                al_free(BufferListStart);
                BufferListStart = next;
            }
            goto done;
        }
    }
    /* All buffers good, unlock them now. */
    BufferList = BufferListStart;
    while(BufferList != NULL)
    {
        ALbuffer *buffer = BufferList->buffer;
        BufferList = BufferList->next;
    }

    /* Source is now streaming */
    source->SourceType = AL_STREAMING;

    if(!(BufferList=source->queue))
        source->queue = BufferListStart;
    else
    {
        ALbufferlistitem *next;
        while((next=BufferList->next) != NULL)
            BufferList = next;
        BufferList->next = BufferListStart;
    }

done:
    ALCcontext_DecRef(context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}

AL_API ALvoid AL_APIENTRY alSourceUnqueueBuffers(ALuint src, ALsizei nb, ALuint *buffers)
{
#if 0
    ALCcontext *context;
    ALsource *source;
    ALbufferlistitem *OldHead;
    ALbufferlistitem *OldTail;
    ALbufferlistitem *Current;
    ALvoice *voice;
    ALsizei i = 0;

    context = GetContextRef();
    if(!context) return;

    if(!(nb >= 0))
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);

    if((source=LookupSource(context, src)) == NULL)
        SET_ERROR_AND_GOTO(context, AL_INVALID_NAME, done);

    /* Nothing to unqueue. */
    if(nb == 0) goto done;

    if(source->Looping || source->SourceType != AL_STREAMING)
    {
        /* Trying to unqueue buffers on a looping or non-streaming source. */
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    }

    /* Find the new buffer queue head */
    OldTail = source->queue;
    Current = NULL;
    if((voice=GetSourceVoice(source, context)) != NULL)
        Current = voice->current_buffer;
    else if(source->state == AL_INITIAL)
        Current = OldTail;
    if(OldTail != Current)
    {
        for(i = 1;i < nb;i++)
        {
            ALbufferlistitem *next = OldTail->next;
            if(!next || next == Current) break;
            OldTail = next;
        }
    }
    if(i != nb)
    {
        /* Trying to unqueue pending buffers. */
        SET_ERROR_AND_GOTO(context, AL_INVALID_VALUE, done);
    }

    /* Swap it, and cut the new head from the old. */
    OldHead = source->queue;
    source->queue = OldTail->next;
    OldTail->next = NULL;

    while(OldHead != NULL)
    {
        ALbufferlistitem *next = OldHead->next;
        ALbuffer *buffer = OldHead->buffer;

        if(!buffer)
            *(buffers++) = 0;
        else
        {
            *(buffers++) = buffer->id;
            buffer->ref -= 1;
        }

        al_free(OldHead);
        OldHead = next;
    }

done:
    ALCcontext_DecRef(context);
#else
    ALCcontext *Context = GetContextRef();
    if(!Context) return;

    alSetError(Context, AL_INVALID_NAME);
#endif
}


void InitSourceParams(ALsource *Source, ALsizei num_sends)
{
    ALsizei i;

    Source->StereoPan[0] = DEG2RAD( 30.0f);
    Source->StereoPan[1] = DEG2RAD(-30.0f);

    Source->Radius = 0.0f;

    Source->Direct.Gain = 1.0f;
    Source->Direct.GainHF = 1.0f;
    Source->Direct.HFReference = LOWPASSFREQREF;
    Source->Direct.GainLF = 1.0f;
    Source->Direct.LFReference = HIGHPASSFREQREF;
    Source->Send = al_calloc(16, num_sends*sizeof(Source->Send[0]));
    for(i = 0;i < num_sends;i++)
    {
        Source->Send[i].Slot = NULL;
        Source->Send[i].Gain = 1.0f;
        Source->Send[i].GainHF = 1.0f;
        Source->Send[i].HFReference = LOWPASSFREQREF;
        Source->Send[i].GainLF = 1.0f;
        Source->Send[i].LFReference = HIGHPASSFREQREF;
    }

    Source->Offset = 0.0;
    Source->OffsetType = AL_NONE;
    Source->SourceType = AL_UNDETERMINED;
    Source->state = AL_INITIAL;

    /* No way to do an 'init' here, so just test+set with relaxed ordering and
     * ignore the test.
     */
    Source->PropsClean = 1;
}

void DeinitSource(ALsource *source, ALsizei num_sends)
{
    ALsizei i;

    if(source->Send)
    {
        for(i = 0;i < num_sends;i++)
        {
            if(source->Send[i].Slot)
                source->Send[i].Slot->ref -= 1;
            source->Send[i].Slot = NULL;
        }
        al_free(source->Send);
        source->Send = NULL;
    }
}

static void UpdateSourceProps(ALsource *source, ALvoice *voice, ALsizei num_sends)
{
    struct ALvoiceProps *props;
    struct ALvoiceProps *temp_props;
    ALsizei i;

    /* Get an unused property container, or allocate a new one as needed. */
    props = voice->FreeList;
    if(!props)
        props = al_calloc(16, FAM_SIZE(struct ALvoiceProps, Send, num_sends));
    else
    {
        struct ALvoiceProps *next;
        do {
            next = props->next;
        } while((voice->FreeList == props ? (voice->FreeList = next, true) : (props = voice->FreeList, false)) == 0);
    }

    /* Copy in current property values. */
    props->StereoPan[0] = source->StereoPan[0];
    props->StereoPan[1] = source->StereoPan[1];

    props->Radius = source->Radius;

    props->Direct.Gain = source->Direct.Gain;
    props->Direct.GainHF = source->Direct.GainHF;
    props->Direct.HFReference = source->Direct.HFReference;
    props->Direct.GainLF = source->Direct.GainLF;
    props->Direct.LFReference = source->Direct.LFReference;

    for(i = 0;i < num_sends;i++)
    {
        props->Send[i].Slot = source->Send[i].Slot;
        props->Send[i].Gain = source->Send[i].Gain;
        props->Send[i].GainHF = source->Send[i].GainHF;
        props->Send[i].HFReference = source->Send[i].HFReference;
        props->Send[i].GainLF = source->Send[i].GainLF;
        props->Send[i].LFReference = source->Send[i].LFReference;
    }

    /* Set the new container for updating internal parameters. */
    temp_props = props;
    props = voice->Update;
    voice->Update = temp_props;;
    if(props)
    {
        /* If there was an unused update container, put it back in the
         * freelist.
         */
        props->next = voice->FreeList;
    }
}

void UpdateAllSourceProps(ALCcontext *context)
{
    ALsizei num_sends = context->Device->NumAuxSends;
    ALsizei pos;

    for(pos = 0;pos < context->VoiceCount;pos++)
    {
        ALvoice *voice = context->Voices[pos];
        ALsource *source = voice->Source;
        int old_props_clean = source->PropsClean;
        source->PropsClean = 1;
        if(source && !old_props_clean)
            UpdateSourceProps(source, voice, num_sends);
    }
}


/* GetSourceSampleOffset
 *
 * Gets the current read offset for the given Source, in 32.32 fixed-point
 * samples. The offset is relative to the start of the queue (not the start of
 * the current buffer).
 */
static ALint64 GetSourceSampleOffset(ALsource *Source, ALCcontext *context, ALuint64 *clocktime)
{
    ALCdevice *device = context->Device;
    ALuint64 readPos;
    ALvoice *voice;

    readPos = 0;
    *clocktime = GetDeviceClockTime(device);

    voice = GetSourceVoice(Source, context);
    if(voice)
    {
        readPos  = (ALuint64)voice->position << 32;
        readPos |= (ALuint64)voice->position_fraction <<
                    (32-FRACTIONBITS);
    }

    if(voice)
    {
        readPos = 0;
    }

    return (ALint64)readPos;
}

/* GetSourceSecOffset
 *
 * Gets the current read offset for the given Source, in seconds. The offset is
 * relative to the start of the queue (not the start of the current buffer).
 */
static ALdouble GetSourceSecOffset(ALsource *Source, ALCcontext *context, ALuint64 *clocktime)
{
    ALCdevice *device = context->Device;
    ALuint64 readPos;
    ALdouble offset;
    ALvoice *voice;

    readPos = 0;
    *clocktime = GetDeviceClockTime(device);

    voice = GetSourceVoice(Source, context);
    if(voice)
    {
        readPos  = (ALuint64)voice->position <<
                    FRACTIONBITS;
        readPos |= voice->position_fraction;
    }

    offset = 0.0;

    return offset;
}

/* GetSourceOffset
 *
 * Gets the current read offset for the given Source, in the appropriate format
 * (Bytes, Samples or Seconds). The offset is relative to the start of the
 * queue (not the start of the current buffer).
 */
static ALdouble GetSourceOffset(ALsource *Source, ALenum name, ALCcontext *context)
{
    ALCdevice *device = context->Device;
    ALuint readPos;
    ALsizei readPosFrac;
    ALdouble offset;

    readPos = readPosFrac = 0;

    offset = 0.0;

    return offset;
}


/* ApplyOffset
 *
 * Apply the stored playback offset to the Source. This function will update
 * the number of buffers "played" given the stored offset.
 */
static ALboolean ApplyOffset(ALsource *Source, ALvoice *voice)
{
    return AL_FALSE;
}


/* GetSampleOffset
 *
 * Retrieves the sample offset into the Source's queue (from the Sample, Byte
 * or Second offset supplied by the application). This takes into account the
 * fact that the buffer format may have been modifed since.
 */
static ALboolean GetSampleOffset(ALsource *Source, ALuint *offset, ALsizei *frac)
{
    return AL_FALSE;
}


/* ReleaseALSources
 *
 * Destroys all sources in the source map.
 */
ALvoid ReleaseALSources(ALCcontext *Context)
{
    ALCdevice *device = Context->Device;
    ALsizei pos;
    for(pos = 0;pos < Context->SourceMap.size;pos++)
    {
        ALsource *temp = Context->SourceMap.values[pos];
        Context->SourceMap.values[pos] = NULL;

        DeinitSource(temp, device->NumAuxSends);

        FreeThunkEntry(temp->id);
        memset(temp, 0, sizeof(*temp));
        al_free(temp);
    }
}
