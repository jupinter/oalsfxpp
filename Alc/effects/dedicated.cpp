/**
 * OpenAL cross platform audio library
 * Copyright (C) 2011 by Chris Robinson.
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
#include "alFilter.h"
#include "alu.h"


typedef struct ALdedicatedState {
    DERIVE_FROM_TYPE(ALeffectState);

    ALfloat gains[MAX_OUTPUT_CHANNELS];
} ALdedicatedState;

static ALvoid ALdedicatedState_Destruct(ALdedicatedState *state);
static ALboolean ALdedicatedState_deviceUpdate(ALdedicatedState *state, ALCdevice *device);
static ALvoid ALdedicatedState_update(ALdedicatedState *state, const ALCdevice *device, const ALeffectslot *Slot, const ALeffectProps *props);
static ALvoid ALdedicatedState_process(ALdedicatedState *state, ALsizei SamplesToDo, const ALfloat (*SamplesIn)[BUFFERSIZE], ALfloat (*SamplesOut)[BUFFERSIZE], ALsizei NumChannels);
DECLARE_DEFAULT_ALLOCATORS(ALdedicatedState)

DEFINE_ALEFFECTSTATE_VTABLE(ALdedicatedState);


static void ALdedicatedState_Construct(ALdedicatedState *state)
{
    ALsizei s;

    ALeffectState_Construct(STATIC_CAST(ALeffectState, state));
    SET_VTABLE2(ALdedicatedState, ALeffectState, state);

    for(s = 0;s < MAX_OUTPUT_CHANNELS;s++)
        state->gains[s] = 0.0f;
}

static ALvoid ALdedicatedState_Destruct(ALdedicatedState *state)
{
    ALeffectState_Destruct(STATIC_CAST(ALeffectState,state));
}

static ALboolean ALdedicatedState_deviceUpdate(ALdedicatedState *state, ALCdevice *device)
{
    return AL_TRUE;
}

static ALvoid ALdedicatedState_update(ALdedicatedState *state, const ALCdevice *device, const ALeffectslot *Slot, const ALeffectProps *props)
{
    ALfloat Gain;
    ALuint i;

    for(i = 0;i < MAX_OUTPUT_CHANNELS;i++)
        state->gains[i] = 0.0f;

    Gain = props->dedicated.gain;
    if(Slot->params.effect_type == AL_EFFECT_DEDICATED_LOW_FREQUENCY_EFFECT)
    {
        int idx;
        if((idx=GetChannelIdxByName(device->real_out, LFE)) != -1)
        {
            STATIC_CAST(ALeffectState,state)->out_buffer = device->real_out.buffer;
            STATIC_CAST(ALeffectState,state)->out_channels = device->real_out.num_channels;
            state->gains[idx] = Gain;
        }
    }
    else if(Slot->params.effect_type == AL_EFFECT_DEDICATED_DIALOGUE)
    {
        int idx;
        /* Dialog goes to the front-center speaker if it exists, otherwise it
         * plays from the front-center location. */
        if((idx=GetChannelIdxByName(device->real_out, FrontCenter)) != -1)
        {
            STATIC_CAST(ALeffectState,state)->out_buffer = device->real_out.buffer;
            STATIC_CAST(ALeffectState,state)->out_channels = device->real_out.num_channels;
            state->gains[idx] = Gain;
        }
        else
        {
            ALfloat coeffs[MAX_AMBI_COEFFS];
            CalcAngleCoeffs(0.0f, 0.0f, 0.0f, coeffs);

            STATIC_CAST(ALeffectState,state)->out_buffer = device->dry.buffer;
            STATIC_CAST(ALeffectState,state)->out_channels = device->dry.num_channels;
            ComputePanningGains(device->dry, coeffs, Gain, state->gains);
        }
    }
}

static ALvoid ALdedicatedState_process(ALdedicatedState *state, ALsizei SamplesToDo, const ALfloat (*SamplesIn)[BUFFERSIZE], ALfloat (*SamplesOut)[BUFFERSIZE], ALsizei NumChannels)
{
    ALsizei i, c;

    SamplesIn = ASSUME_ALIGNED(SamplesIn, 16);
    SamplesOut = ASSUME_ALIGNED(SamplesOut, 16);
    for(c = 0;c < NumChannels;c++)
    {
        const ALfloat gain = state->gains[c];
        if(!(fabsf(gain) > GAIN_SILENCE_THRESHOLD))
            continue;

        for(i = 0;i < SamplesToDo;i++)
            SamplesOut[c][i] += SamplesIn[0][i] * gain;
    }
}


typedef struct ALdedicatedStateFactory {
    DERIVE_FROM_TYPE(ALeffectStateFactory);
} ALdedicatedStateFactory;

ALeffectState *ALdedicatedStateFactory_create(ALdedicatedStateFactory *factory)
{
    ALdedicatedState *state;

    NEW_OBJ0(state, ALdedicatedState)();
    if(!state) return NULL;

    return STATIC_CAST(ALeffectState, state);
}

DEFINE_ALEFFECTSTATEFACTORY_VTABLE(ALdedicatedStateFactory);


ALeffectStateFactory *ALdedicatedStateFactory_getFactory(void)
{
    static ALdedicatedStateFactory DedicatedFactory = { { GET_VTABLE2(ALdedicatedStateFactory, ALeffectStateFactory) } };

    return STATIC_CAST(ALeffectStateFactory, &DedicatedFactory);
}