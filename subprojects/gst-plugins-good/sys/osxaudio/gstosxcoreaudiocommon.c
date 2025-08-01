/*
 * GStreamer
 * Copyright (C) 2012-2013 Fluendo S.A. <support@fluendo.com>
 *   Authors: Josep Torra Vallès <josep@fluendo.com>
 *            Andoni Morales Alastruey <amorales@fluendo.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "gstosxcoreaudiocommon.h"

GST_DEBUG_CATEGORY_EXTERN (osx_coreaudio_debug);
#define GST_CAT_DEFAULT osx_coreaudio_debug

void
gst_core_audio_remove_render_callback (GstCoreAudio * core_audio)
{
  AURenderCallbackStruct input;
  OSStatus status;
  AudioUnitPropertyID callback_type;

  /* Deactivate the render callback by calling SetRenderCallback
   * with a NULL inputProc.
   */
  input.inputProc = NULL;
  input.inputProcRefCon = NULL;
  callback_type = core_audio->is_src ?
      kAudioOutputUnitProperty_SetInputCallback :
      kAudioUnitProperty_SetRenderCallback;

  status = AudioUnitSetProperty (core_audio->audiounit, callback_type, kAudioUnitScope_Global, 0,       /* N/A for global */
      &input, sizeof (input));

  if (status) {
    GST_WARNING_OBJECT (core_audio->osxbuf,
        "Failed to remove render callback %d", (int) status);
  }

  /* Remove the RenderNotify too */
  status = AudioUnitRemoveRenderNotify (core_audio->audiounit,
      (AURenderCallback) gst_core_audio_render_notify, core_audio);

  if (status) {
    GST_WARNING_OBJECT (core_audio->osxbuf,
        "Failed to remove render notify callback %d", (int) status);
  }

  /* We're deactivated.. */
  core_audio->io_proc_needs_deactivation = FALSE;
  core_audio->io_proc_active = FALSE;
}

OSStatus
gst_core_audio_render_notify (GstCoreAudio * core_audio,
    AudioUnitRenderActionFlags * ioActionFlags,
    const AudioTimeStamp * inTimeStamp,
    unsigned int inBusNumber,
    unsigned int inNumberFrames, AudioBufferList * ioData)
{
  /* Before rendering a frame, we get the PreRender notification.
   * Here, we detach the RenderCallback if we've been paused.
   *
   * This is necessary (rather than just directly detaching it) to
   * work around some thread-safety issues in CoreAudio
   */
  if ((*ioActionFlags) & kAudioUnitRenderAction_PreRender) {
    if (core_audio->io_proc_needs_deactivation) {
      gst_core_audio_remove_render_callback (core_audio);
    }
  }

  return noErr;
}

gboolean
gst_core_audio_io_proc_start (GstCoreAudio * core_audio)
{
  OSStatus status;
  AURenderCallbackStruct input;
  AudioUnitPropertyID callback_type;

  GST_DEBUG_OBJECT (core_audio->osxbuf,
      "osx ring buffer start ioproc: %p device_id %lu",
      core_audio->element->io_proc, (gulong) core_audio->device_id);
  if (!core_audio->io_proc_active) {
    callback_type = core_audio->is_src ?
        kAudioOutputUnitProperty_SetInputCallback :
        kAudioUnitProperty_SetRenderCallback;

    input.inputProc = (AURenderCallback) core_audio->element->io_proc;
    input.inputProcRefCon = core_audio->osxbuf;

    status = AudioUnitSetProperty (core_audio->audiounit, callback_type, kAudioUnitScope_Global, 0,     /* N/A for global */
        &input, sizeof (input));

    if (status) {
      GST_ERROR_OBJECT (core_audio->osxbuf,
          "AudioUnitSetProperty failed: %d", (int) status);
      return FALSE;
    }
    // ### does it make sense to do this notify stuff for input mode?
    status = AudioUnitAddRenderNotify (core_audio->audiounit,
        (AURenderCallback) gst_core_audio_render_notify, core_audio);

    if (status) {
      GST_ERROR_OBJECT (core_audio->osxbuf,
          "AudioUnitAddRenderNotify failed %d", (int) status);
      return FALSE;
    }
    core_audio->io_proc_active = TRUE;
  }

  core_audio->io_proc_needs_deactivation = FALSE;

  // AudioOutputUnitStart on iOS can wait for the render callback to finish,
  // where in our case we set the ringbuffer timestamp, which also needs the ringbuf lock.
  GST_OBJECT_UNLOCK (core_audio->osxbuf);
  status = AudioOutputUnitStart (core_audio->audiounit);
  GST_OBJECT_LOCK (core_audio->osxbuf);
  if (status) {
    GST_ERROR_OBJECT (core_audio->osxbuf, "AudioOutputUnitStart failed: %d",
        (int) status);
    return FALSE;
  }
  return TRUE;
}

gboolean
gst_core_audio_io_proc_stop (GstCoreAudio * core_audio)
{
  OSErr status;

  GST_DEBUG_OBJECT (core_audio->osxbuf,
      "osx ring buffer stop ioproc: %p device_id %lu",
      core_audio->element->io_proc, (gulong) core_audio->device_id);

  status = AudioOutputUnitStop (core_audio->audiounit);
  if (status) {
    GST_WARNING_OBJECT (core_audio->osxbuf,
        "AudioOutputUnitStop failed: %d", (int) status);
  }
  // ###: why is it okay to directly remove from here but not from pause() ?
  if (core_audio->io_proc_active) {
    gst_core_audio_remove_render_callback (core_audio);
  }
  return TRUE;
}

AudioBufferList *
buffer_list_alloc (UInt32 channels, UInt32 size, gboolean interleaved)
{
  AudioBufferList *list;
  gsize list_size;
  UInt32 num_buffers, n;

  num_buffers = interleaved ? 1 : channels;
  /* AudioBufferList member mBuffers is variable-length array */
  list_size = G_STRUCT_OFFSET (AudioBufferList, mBuffers[num_buffers]);
  list = (AudioBufferList *) g_malloc (list_size);

  list->mNumberBuffers = num_buffers;
  for (n = 0; n < num_buffers; ++n) {
    /* See http://lists.apple.com/archives/coreaudio-api/2015/Feb/msg00027.html */
    list->mBuffers[n].mNumberChannels = interleaved ? channels : 1;
    /* AudioUnitRender will keep overwriting mDataByteSize */
    list->mBuffers[n].mDataByteSize = size;
    list->mBuffers[n].mData = g_malloc (size);
  }

  return list;
}

void
buffer_list_free (AudioBufferList * list)
{
  UInt32 n;

  if (list == NULL)
    return;

  for (n = 0; n < list->mNumberBuffers; ++n) {
    g_free (list->mBuffers[n].mData);
  }

  g_free (list);
}

gboolean
gst_core_audio_bind_device (GstCoreAudio * core_audio)
{
  OSStatus status;

  /* Specify which device we're using. */
  GST_DEBUG_OBJECT (core_audio->osxbuf, "Bind AudioUnit to device %s",
      core_audio->unique_id);
  status = AudioUnitSetProperty (core_audio->audiounit,
      kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
      &core_audio->device_id, sizeof (AudioDeviceID));
  if (status) {
    GST_ERROR_OBJECT (core_audio->osxbuf, "Failed binding to device: %d",
        (int) status);
    goto audiounit_error;
  }
  return TRUE;

audiounit_error:
  if (core_audio->recBufferList) {
    buffer_list_free (core_audio->recBufferList);
    core_audio->recBufferList = NULL;
  }
  return FALSE;
}

static gboolean
_core_audio_set_property (GstCoreAudio * core_audio, AudioUnitPropertyID inID,
    void *inData, UInt32 inDataSize)
{
  OSStatus status;
  AudioUnitScope scope;
  AudioUnitElement element;

  scope = CORE_AUDIO_INNER_SCOPE (core_audio);
  element = CORE_AUDIO_ELEMENT (core_audio);

  status =
      AudioUnitSetProperty (core_audio->audiounit, inID, scope, element, inData,
      inDataSize);

  if (status != noErr) {
    GST_WARNING_OBJECT (core_audio->osxbuf,
        "Failed to set Audio Unit property: %d", (int) status);
    return FALSE;;
  }

  return TRUE;
}

/* The AudioUnit must be uninitialized before calling this */
gboolean
gst_core_audio_set_channel_layout (GstCoreAudio * core_audio,
    gint channels, GstCaps * caps)
{
  AudioChannelLayout *layout = NULL;
  gboolean ret;
  gsize layoutSize;
  gint i;
  GstStructure *structure;
  GstAudioChannelPosition positions[GST_OSX_AUDIO_MAX_CHANNEL];
  guint64 channel_mask;

  g_return_val_if_fail (channels <= GST_OSX_AUDIO_MAX_CHANNEL, FALSE);

  /* Determine the channel positions */
  structure = gst_caps_get_structure (caps, 0);
  channel_mask = 0;
  gst_structure_get (structure, "channel-mask", GST_TYPE_BITMASK, &channel_mask,
      NULL);

  if (channel_mask != 0)
    gst_audio_channel_positions_from_mask (channels, channel_mask, positions);

  /* AudioChannelLayout member mChannelDescriptions is variable-length array */
  layoutSize =
      G_STRUCT_OFFSET (AudioChannelLayout, mChannelDescriptions[channels]);
  layout = g_malloc (layoutSize);

  /* Fill out the AudioChannelLayout */
  layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
  layout->mChannelBitmap = 0;   /* Not used */
  layout->mNumberChannelDescriptions = channels;
  for (i = 0; i < channels; i++) {
    if (channel_mask != 0) {
      layout->mChannelDescriptions[i].mChannelLabel =
          gst_audio_channel_position_to_core_audio (positions[i], i);
    } else {
      /* Discrete channel numbers are ORed into this */
      layout->mChannelDescriptions[i].mChannelLabel =
          kAudioChannelLabel_Discrete_0 | i;
    }

    /* Others unused */
    layout->mChannelDescriptions[i].mChannelFlags = kAudioChannelFlags_AllOff;
    layout->mChannelDescriptions[i].mCoordinates[0] = 0.f;
    layout->mChannelDescriptions[i].mCoordinates[1] = 0.f;
    layout->mChannelDescriptions[i].mCoordinates[2] = 0.f;
  }

  /* Sets GStreamer-ordered channel layout on the inner scope.
   * Reordering between the inner scope and outer scope is handled
   * by the Audio Unit itself. */
  ret = _core_audio_set_property (core_audio,
      kAudioUnitProperty_AudioChannelLayout, layout, layoutSize);

  g_free (layout);
  return ret;
}

/* The AudioUnit must be uninitialized before calling this */
gboolean
gst_core_audio_set_format (GstCoreAudio * core_audio,
    AudioStreamBasicDescription format)
{
  GST_DEBUG_OBJECT (core_audio->osxbuf, "Setting format for AudioUnit");

  return _core_audio_set_property (core_audio, kAudioUnitProperty_StreamFormat,
      &format, sizeof (AudioStreamBasicDescription));
}

gboolean
gst_core_audio_open_device (GstCoreAudio * core_audio, OSType sub_type,
    const gchar * adesc)
{
  AudioComponentDescription desc;
  AudioComponent comp;
  OSStatus status;
  AudioUnit unit;
  UInt32 enableIO;

  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = sub_type;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  desc.componentFlags = 0;
  desc.componentFlagsMask = 0;

  comp = AudioComponentFindNext (NULL, &desc);

  if (comp == NULL) {
    GST_WARNING_OBJECT (core_audio->osxbuf, "Couldn't find %s component",
        adesc);
    return FALSE;
  }

  status = AudioComponentInstanceNew (comp, &unit);

  if (status) {
    GST_ERROR_OBJECT (core_audio->osxbuf, "Couldn't open %s component %d",
        adesc, (int) status);
    return FALSE;
  }

  if (core_audio->is_src) {
    /* enable input */
    enableIO = 1;
    status = AudioUnitSetProperty (unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,   /* 1 = input element */
        &enableIO, sizeof (enableIO));

    if (status) {
      AudioComponentInstanceDispose (unit);
      GST_WARNING_OBJECT (core_audio->osxbuf, "Failed to enable input: %d",
          (int) status);
      return FALSE;
    }

    /* disable output */
    enableIO = 0;
    status = AudioUnitSetProperty (unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,  /* 0 = output element */
        &enableIO, sizeof (enableIO));

    if (status) {
      AudioComponentInstanceDispose (unit);
      GST_WARNING_OBJECT (core_audio->osxbuf, "Failed to disable output: %d",
          (int) status);
      return FALSE;
    }
  }

  GST_DEBUG_OBJECT (core_audio->osxbuf, "Created %s AudioUnit: %p", adesc,
      unit);
  core_audio->audiounit = unit;
  return TRUE;
}

AudioChannelLabel
gst_audio_channel_position_to_core_audio (GstAudioChannelPosition
    position, int channel)
{
  switch (position) {
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT:
      return kAudioChannelLabel_Left;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT:
      return kAudioChannelLabel_Right;
    case GST_AUDIO_CHANNEL_POSITION_REAR_CENTER:
      return kAudioChannelLabel_CenterSurround;
    case GST_AUDIO_CHANNEL_POSITION_REAR_LEFT:
      return kAudioChannelLabel_RearSurroundLeft;
    case GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT:
      return kAudioChannelLabel_RearSurroundRight;
    case GST_AUDIO_CHANNEL_POSITION_LFE1:
      return kAudioChannelLabel_LFEScreen;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER:
      return kAudioChannelLabel_Center;
    case GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT:
      return kAudioChannelLabel_LeftSurroundDirect;
    case GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT:
      return kAudioChannelLabel_RightSurroundDirect;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:
      return kAudioChannelLabel_LeftCenter;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER:
      return kAudioChannelLabel_RightCenter;
    case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT:
      return kAudioChannelLabel_TopBackLeft;
    case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER:
      return kAudioChannelLabel_TopBackCenter;
    case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT:
      return kAudioChannelLabel_TopBackRight;
    case GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT:
      return kAudioChannelLabel_LeftWide;
    case GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT:
      return kAudioChannelLabel_RightWide;
    case GST_AUDIO_CHANNEL_POSITION_LFE2:
      return kAudioChannelLabel_LFE2;
    case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT:
      return kAudioChannelLabel_VerticalHeightLeft;
    case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT:
      return kAudioChannelLabel_VerticalHeightRight;
    case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER:
      return kAudioChannelLabel_VerticalHeightCenter;
    case GST_AUDIO_CHANNEL_POSITION_TOP_CENTER:
      return kAudioChannelLabel_TopCenterSurround;
    case GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT:
      return kAudioChannelLabel_LeftSurround;
    case GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT:
      return kAudioChannelLabel_RightSurround;

      /* Special position values */
    case GST_AUDIO_CHANNEL_POSITION_NONE:
      return kAudioChannelLabel_Discrete_0 | channel;
    case GST_AUDIO_CHANNEL_POSITION_MONO:
      return kAudioChannelLabel_Mono;

      /* Following positions are unmapped --
       * i.e. mapped to kAudioChannelLabel_Unknown: */
    case GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_LEFT:
    case GST_AUDIO_CHANNEL_POSITION_TOP_SIDE_RIGHT:
    case GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_CENTER:
    case GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_LEFT:
    case GST_AUDIO_CHANNEL_POSITION_BOTTOM_FRONT_RIGHT:
    default:
      return kAudioChannelLabel_Unknown;
  }
}

/* Performs a best-effort conversion. 'channel' is used for warnings only. */
GstAudioChannelPosition
gst_core_audio_channel_label_to_gst (AudioChannelLabel label,
    int channel, gboolean warn, AudioDeviceID device_id)
{
  switch (label) {
    case kAudioChannelLabel_Left:
      return GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT;
    case kAudioChannelLabel_Right:
      return GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT;
    case kAudioChannelLabel_Center:
      return GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER;
    case kAudioChannelLabel_LFEScreen:
      return GST_AUDIO_CHANNEL_POSITION_LFE1;
    case kAudioChannelLabel_LeftSurround:
      return GST_AUDIO_CHANNEL_POSITION_SURROUND_LEFT;
    case kAudioChannelLabel_RightSurround:
      return GST_AUDIO_CHANNEL_POSITION_SURROUND_RIGHT;
    case kAudioChannelLabel_LeftSurroundDirect:
      return GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT;
    case kAudioChannelLabel_RightSurroundDirect:
      return GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT;
    case kAudioChannelLabel_CenterSurround:
      return GST_AUDIO_CHANNEL_POSITION_REAR_CENTER;
    case kAudioChannelLabel_LeftCenter:
      return GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
    case kAudioChannelLabel_RightCenter:
      return GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
    case kAudioChannelLabel_TopBackLeft:
      return GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT;
    case kAudioChannelLabel_TopBackCenter:
      return GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER;
    case kAudioChannelLabel_TopBackRight:
      return GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT;
    case kAudioChannelLabel_LeftWide:
      return GST_AUDIO_CHANNEL_POSITION_WIDE_LEFT;
    case kAudioChannelLabel_RightWide:
      return GST_AUDIO_CHANNEL_POSITION_WIDE_RIGHT;
    case kAudioChannelLabel_LFE2:
      return GST_AUDIO_CHANNEL_POSITION_LFE2;
    case kAudioChannelLabel_VerticalHeightLeft:
      return GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT;
    case kAudioChannelLabel_VerticalHeightRight:
      return GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT;
    case kAudioChannelLabel_VerticalHeightCenter:
      return GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER;
    case kAudioChannelLabel_RearSurroundLeft:
      return GST_AUDIO_CHANNEL_POSITION_REAR_LEFT;
    case kAudioChannelLabel_RearSurroundRight:
      return GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT;
    case kAudioChannelLabel_TopCenterSurround:
      return GST_AUDIO_CHANNEL_POSITION_TOP_CENTER;

      /* Special position values */

    case kAudioChannelLabel_Mono:
      /* GST_AUDIO_CHANNEL_POSITION_MONO is only for 1-channel layouts */
      return GST_AUDIO_CHANNEL_POSITION_INVALID;
    case kAudioChannelLabel_Discrete:
      return GST_AUDIO_CHANNEL_POSITION_NONE;

      /*
         Following labels are unmapped --
         i.e. mapped to GST_AUDIO_CHANNEL_POSITION_INVALID:
       */
    case kAudioChannelLabel_LeftTotal:
    case kAudioChannelLabel_RightTotal:
    case kAudioChannelLabel_HearingImpaired:
    case kAudioChannelLabel_Narration:
    case kAudioChannelLabel_DialogCentricMix:
    case kAudioChannelLabel_CenterSurroundDirect:
    case kAudioChannelLabel_Haptic:
    default:
      if (label >> 16 != 0) {   /* kAudioChannelLabel_Discrete_N */
        /* no way to store discrete channel order */
        if (warn)
          GST_WARNING
              ("Device %d has kAudioChannelLabel_Discrete_N channels, order will be lost",
              device_id);
        return GST_AUDIO_CHANNEL_POSITION_NONE;
      } else {
        if (warn)
          GST_WARNING
              ("Device %d channel %u has unsupported label %d, skipping",
              device_id, channel, (int) label);
        return GST_AUDIO_CHANNEL_POSITION_INVALID;
      }
  }
}

void
gst_core_audio_dump_channel_layout (AudioChannelLayout * channel_layout)
{
  UInt32 i;

  GST_DEBUG ("mChannelLayoutTag: 0x%lx",
      (unsigned long) channel_layout->mChannelLayoutTag);
  GST_DEBUG ("mChannelBitmap: 0x%lx",
      (unsigned long) channel_layout->mChannelBitmap);
  GST_DEBUG ("mNumberChannelDescriptions: %lu",
      (unsigned long) channel_layout->mNumberChannelDescriptions);
  for (i = 0; i < channel_layout->mNumberChannelDescriptions; i++) {
    AudioChannelDescription *channel_desc =
        &channel_layout->mChannelDescriptions[i];
    GST_DEBUG ("  mChannelLabel: 0x%lx mChannelFlags: 0x%lx "
        "mCoordinates[0]: %f mCoordinates[1]: %f "
        "mCoordinates[2]: %f",
        (unsigned long) channel_desc->mChannelLabel,
        (unsigned long) channel_desc->mChannelFlags,
        channel_desc->mCoordinates[0], channel_desc->mCoordinates[1],
        channel_desc->mCoordinates[2]);
  }
}

#ifndef HAVE_IOS
gboolean
gst_core_audio_change_ringbuf_device (GstOsxAudioRingBuffer * ringbuf,
    const char *unique_id, AudioDeviceID device_id, gboolean is_src)
{
  GstAudioRingBuffer *base_ringbuf = GST_AUDIO_RING_BUFFER (ringbuf);
  GstCoreAudio *core_audio = ringbuf->core_audio;
  AudioDeviceID old_device_id = core_audio->device_id;
  gboolean old_is_default = core_audio->is_default;
  char *old_unique_id = g_strdup (core_audio->unique_id);
  gboolean ret = TRUE;

  /* Locking the ringbuf should be enough. Anything accessing buf->core_audio
   * should also lock the ringbuf already (aside from SPDIF/passthrough mode,
   * in which we don't support switching on-the-fly). */
  GST_OBJECT_LOCK (ringbuf);

  if (core_audio->is_passthrough) {
    GST_WARNING_OBJECT (core_audio, "Cannot change device in passthrough mode");
    ret = FALSE;
    goto finish;
  }

  if (old_device_id == device_id) {
    GST_DEBUG_OBJECT (core_audio, "Already using device %s (%d)",
        old_unique_id, (int) device_id);
    goto finish;
  }

  if (g_strcmp0 (old_unique_id, unique_id) == 0) {
    GST_DEBUG_OBJECT (core_audio, "Already using device %s (%d)", unique_id,
        (int) old_device_id);
    goto finish;
  }

  core_audio->device_id = device_id;
  g_free (core_audio->unique_id);
  core_audio->unique_id = g_strdup (unique_id);

  if (!gst_core_audio_select_device (core_audio)) {
    /* This doesn't change is_default/unique_id unless it succeeds */
    GST_ERROR_OBJECT (core_audio, "Device %d not found or not usable",
        (int) device_id);
    core_audio->device_id = old_device_id;
    ret = FALSE;
    goto finish;
  }

  if (core_audio->audiounit == NULL) {
    /* gst_osx_audio_ring_buffer_open_device() hasn't been called yet,
     * we're not switching live */
    GST_DEBUG_OBJECT (core_audio,
        "No AudioUnit initialized yet, nothing to change");
    goto finish;
  }

  if (is_src) {
    /* For inputs we always have to recreate the ringbuffer, as AudioUnitRender()
     * doesn't want to resample. Set the flag and reinitialize in create(). */
    core_audio->device_change_pending = TRUE;
    goto finish;
  }

  if (!gst_core_audio_bind_device (core_audio)) {
    /* When bind_device() fails, AudioUnit will usually shut itself down,
     * so we need to bind back to the old device and restart our unit */
    ret = FALSE;
    if (unique_id)
      GST_ERROR_OBJECT (core_audio, "Failed to bind to device %s, reverting",
          unique_id);
    else
      GST_ERROR_OBJECT (core_audio, "Failed to bind to device %i, reverting",
          (int) device_id);

    /* select_device() changes these, so let's rewind */
    core_audio->device_id = old_device_id;
    core_audio->is_default = old_is_default;
    g_free (core_audio->unique_id);
    core_audio->unique_id = old_unique_id;

    /* This can also fail in very rare cases (e.g. the original device got unplugged 
     * in the meantime), stop the ringbuffer and shut things down when that happens */
    if (!gst_core_audio_bind_device (core_audio)
        || !gst_core_audio_start_processing (core_audio)) {
      GST_ERROR_OBJECT (core_audio, "Failed to revert to device %s (%d)",
          old_unique_id, (int) old_device_id);

      gst_audio_ring_buffer_set_errored (base_ringbuf);
      GST_AUDIO_RING_BUFFER_SIGNAL (base_ringbuf);
    } else {
      GST_DEBUG_OBJECT (core_audio, "Reverted to device %s (%d)",
          old_unique_id, (int) old_device_id);
    }

    /* We transferred ownership to core_audio above */
    old_unique_id = NULL;
  } else {
    GST_DEBUG_OBJECT (core_audio, "Changed active device to %d",
        (int) device_id);
  }

finish:
  GST_OBJECT_UNLOCK (ringbuf);
  g_free (old_unique_id);
  return ret;
}

char *
gst_core_audio_device_get_prop_str (AudioDeviceID device_id,
    AudioObjectPropertyElement prop_id)
{
  OSStatus status = noErr;
  UInt32 propertySize = 0;
  CFStringRef prop_val;
  char *result = NULL;

  AudioObjectPropertyAddress propAddress = {
    prop_id,
    kAudioDevicePropertyScopeOutput,
    kAudioObjectPropertyElementMain
  };

  propAddress.mScope = kAudioObjectPropertyScopeGlobal;

  /* Get the length of the device name */
  status = AudioObjectGetPropertyDataSize (device_id,
      &propAddress, 0, NULL, &propertySize);
  if (status != noErr) {
    goto beach;
  }

  /* Get the requested property */
  status = AudioObjectGetPropertyData (device_id,
      &propAddress, 0, NULL, &propertySize, &prop_val);
  if (status != noErr) {
    goto beach;
  }

  /* Convert to UTF-8 C String */
  CFIndex prop_len = CFStringGetLength (prop_val);
  CFIndex max_size =
      CFStringGetMaximumSizeForEncoding (prop_len, kCFStringEncodingUTF8) + 1;
  result = g_malloc (max_size);

  if (!CFStringGetCString (prop_val, result, max_size, kCFStringEncodingUTF8)) {
    g_free (result);
    result = NULL;
  }

  CFRelease (prop_val);

beach:
  return result;
}

UInt32
gst_core_audio_device_get_prop_uint32 (AudioDeviceID device_id,
    AudioObjectPropertyElement prop_id)
{
  OSStatus status = noErr;
  UInt32 propertySize = sizeof (UInt32);
  UInt32 result = UINT32_MAX;

  AudioObjectPropertyAddress propAddress = {
    .mSelector = prop_id,
    .mScope = kAudioObjectPropertyScopeGlobal,
    .mElement = kAudioObjectPropertyElementMain,
  };

  /* Get the requested property */
  status = AudioObjectGetPropertyData (device_id,
      &propAddress, 0, NULL, &propertySize, &result);
  if (status != noErr) {
    GST_WARNING ("Device %i, error code %i while fetching property %"
        GST_FOURCC_FORMAT, device_id, status,
        GST_FOURCC_ARGS (GUINT32_FROM_BE (prop_id)));
  }

  return result;
}
#endif

GstClockTime
host_current_time_ns (GstCoreAudio * core_audio)
{
  guint64 mach_t = mach_absolute_time ();
  return gst_util_uint64_scale (mach_t, core_audio->timebase.numer,
      core_audio->timebase.denom);
}

GstClockTime
host_time_to_ns (GstCoreAudio * core_audio, uint64_t host_time)
{
  return gst_util_uint64_scale (host_time, core_audio->timebase.numer,
      core_audio->timebase.denom);
}
