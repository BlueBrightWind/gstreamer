/*
 * GStreamer
 * Copyright (C) 2024 Igalia, S.L.
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstvkencoder-private.h"

#include "gstvkvideo-private.h"

enum
{
  PROP_0,
  PROP_RATE_CONTROL,
  PROP_AVERAGE_BITRATE,
  PROP_QUALITY_LEVEL,
  PROP_MAX
};

static GParamSpec *properties[PROP_MAX];

extern const VkExtensionProperties vk_codec_extensions[3];

extern const uint32_t _vk_codec_supported_extensions[4];

typedef struct _GstVulkanEncoderPrivate GstVulkanEncoderPrivate;

struct _GstVulkanEncoderPrivate
{
  GstVulkanHandle *session_params;

  GstCaps *profile_caps;

  GstVulkanOperation *exec;

  GstVulkanVideoSession session;
  GstVulkanVideoCapabilities caps;
  VkVideoFormatPropertiesKHR format;
  VkVideoEncodeCapabilitiesKHR enc_caps;
  VkVideoEncodeRateControlInfoKHR rate_control_info;

  GstVulkanVideoProfile profile;

  gboolean vk_loaded;
  GstVulkanVideoFunctions vk;

  gint current_slot_index;

  gboolean started;
  gboolean first_encode_cmd;
  struct
  {
    guint rate_control;
    guint average_bitrate;
    guint quality_level;
  } prop;

  guint out_buffer_size_aligned;
  gboolean layered_dpb;
  GstBufferPool *dpb_pool;
  GstBuffer *layered_buffer;
  GstVulkanImageView *layered_view;
};

/**
 * SECTION:vkencoder
 * @title: GstVulkanEncoder
 * @short_description: Generic Vulkan Video Encoder
 */

#define GST_CAT_DEFAULT gst_vulkan_encoder_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define gst_vulkan_encoder_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstVulkanEncoder, gst_vulkan_encoder,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstVulkanEncoder)
    GST_DEBUG_CATEGORY_INIT (gst_vulkan_encoder_debug,
        "vulkanencoder", 0, "Vulkan device encoder"));

const uint32_t _vk_codec_supported_extensions[] = {
  [GST_VK_VIDEO_EXTENSION_ENCODE_H264] = VK_MAKE_VIDEO_STD_VERSION (0, 9, 11),
  [GST_VK_VIDEO_EXTENSION_ENCODE_H265] = VK_MAKE_VIDEO_STD_VERSION (0, 9, 12),
};

#define GST_TYPE_VULKAN_ENCODE_RATE_CONTROL_MODE (gst_vulkan_enc_rate_control_mode_get_type ())
static GType
gst_vulkan_enc_rate_control_mode_get_type (void)
{
  static GType qtype = 0;

  if (qtype == 0) {
    static const GEnumValue values[] = {
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR, "default", "default"},
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR,
            "Rate control is disabled",
          "disabled"},
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR,
            "Constant bitrate mode rate control mode",
          "cbr"},
      {VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR,
            "Variable bitrate mode rate control mode",
          "vbr"},
      {0, NULL, NULL}
    };

    qtype = g_enum_register_static ("GstVulkanEncRateControlMode", values);
  }
  return qtype;
}

static gboolean
_populate_function_table (GstVulkanEncoder * self)
{
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GstVulkanInstance *instance;

  if (priv->vk_loaded)
    return TRUE;

  instance = gst_vulkan_device_get_instance (self->queue->device);
  if (!instance) {
    GST_ERROR_OBJECT (self, "Failed to get instance from the device");
    return FALSE;
  }

  priv->vk_loaded = gst_vulkan_video_get_vk_functions (instance, &priv->vk);
  gst_object_unref (instance);
  return priv->vk_loaded;
}

static void
gst_vulkan_encoder_finalize (GObject * object)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (object);

  gst_clear_object (&self->queue);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_vulkan_encoder_init (GstVulkanEncoder * self)
{
}

static void
gst_vulkan_encoder_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (object);
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_RATE_CONTROL:
      priv->prop.rate_control = g_value_get_enum (value);
      break;
    case PROP_AVERAGE_BITRATE:
      priv->prop.average_bitrate = g_value_get_uint (value);
      break;
    case PROP_QUALITY_LEVEL:
      priv->prop.quality_level = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_vulkan_encoder_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstVulkanEncoder *self = GST_VULKAN_ENCODER (object);
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_RATE_CONTROL:
      g_value_set_enum (value, priv->prop.rate_control);
      break;
    case PROP_AVERAGE_BITRATE:
      g_value_set_uint (value, priv->prop.average_bitrate);
      break;
    case PROP_QUALITY_LEVEL:
      g_value_set_uint (value, priv->prop.quality_level);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_vulkan_encoder_class_init (GstVulkanEncoderClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  gint n_props = PROP_MAX;
  GParamFlags param_flags =
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_CONSTRUCT;

  gobject_class->finalize = gst_vulkan_encoder_finalize;
  gobject_class->set_property = gst_vulkan_encoder_set_property;
  gobject_class->get_property = gst_vulkan_encoder_get_property;

  properties[PROP_RATE_CONTROL] =
      g_param_spec_enum ("rate-control", "Vulkan rate control",
      "Choose the vulkan rate control",
      GST_TYPE_VULKAN_ENCODE_RATE_CONTROL_MODE,
      VK_VIDEO_ENCODE_CONTENT_DEFAULT_KHR, param_flags);

  properties[PROP_AVERAGE_BITRATE] =
      g_param_spec_uint ("average-bitrate", "Vulkan encode average bitrate",
      "Choose the vulkan average encoding bitrate", 0, UINT_MAX, 0,
      param_flags);

  properties[PROP_QUALITY_LEVEL] =
      g_param_spec_uint ("quality-level", "Vulkan encode quality level",
      "Choose the vulkan encoding quality level", 0, UINT_MAX, 0, param_flags);

  g_object_class_install_properties (gobject_class, n_props, properties);
}

static VkFormat
gst_vulkan_video_encoder_get_format (GstVulkanEncoder * self,
    VkImageUsageFlagBits imageUsage, GError ** error)
{
  VkResult res;
  VkVideoFormatPropertiesKHR *fmts = NULL;
  guint i, n_fmts;
  VkPhysicalDevice gpu =
      gst_vulkan_device_get_physical_device (self->queue->device);
  GstVulkanEncoderPrivate *priv =
      gst_vulkan_encoder_get_instance_private (self);
  GstVideoFormat format = GST_VIDEO_FORMAT_UNKNOWN;
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  VkVideoProfileListInfoKHR profile_list = {
    .sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,
    .profileCount = 1,
    .pProfiles = &priv->profile.profile,
  };
  VkPhysicalDeviceVideoFormatInfoKHR fmt_info = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,
    .pNext = &profile_list,
    .imageUsage = imageUsage,
  };

  res = priv->vk.GetPhysicalDeviceVideoFormatProperties (gpu, &fmt_info,
      &n_fmts, NULL);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkGetPhysicalDeviceVideoFormatPropertiesKHR") != VK_SUCCESS)
    goto beach;

  if (n_fmts == 0) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "Profile doesn't have an output format");
    return vk_format;
  }

  fmts = g_new0 (VkVideoFormatPropertiesKHR, n_fmts);
  for (i = 0; i < n_fmts; i++)
    fmts[i].sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;

  res = priv->vk.GetPhysicalDeviceVideoFormatProperties (gpu, &fmt_info,
      &n_fmts, fmts);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkGetPhysicalDeviceVideoFormatPropertiesKHR") != VK_SUCCESS) {
    goto beach;
  }

  if (n_fmts == 0) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "Profile doesn't have an output format");
    goto beach;
  }

  /* find the best output format */
  for (i = 0; i < n_fmts; i++) {
    format = gst_vulkan_format_to_video_format (fmts[i].format);
    if (format == GST_VIDEO_FORMAT_UNKNOWN) {
      GST_WARNING_OBJECT (self, "Unknown Vulkan format %i", fmts[i].format);
      continue;
    } else {
      vk_format = fmts[i].format;
      priv->format = fmts[i];
      break;
    }
  }

  if (vk_format == VK_FORMAT_UNDEFINED) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "No valid output format found");
  }

beach:
  g_clear_pointer (&fmts, g_free);
  return vk_format;
}

static void
gst_vulkan_handle_free_video_session_parameters (GstVulkanHandle * handle,
    gpointer data)
{
  PFN_vkDestroyVideoSessionParametersKHR vkDestroyVideoSessionParameters;

  g_return_if_fail (handle != NULL);
  g_return_if_fail (handle->handle != VK_NULL_HANDLE);
  g_return_if_fail (handle->type ==
      GST_VULKAN_HANDLE_TYPE_VIDEO_SESSION_PARAMETERS);
  g_return_if_fail (handle->user_data);

  vkDestroyVideoSessionParameters = handle->user_data;
  vkDestroyVideoSessionParameters (handle->device->device,
      (VkVideoSessionKHR) handle->handle, NULL);
}

static GstVulkanHandle *
gst_vulkan_encoder_new_video_session_parameters (GstVulkanEncoder * self,
    GstVulkanEncoderParameters * params, GError ** error)
{
  GstVulkanEncoderPrivate *priv;
  VkVideoSessionParametersCreateInfoKHR session_params_info;
  VkResult res;
  VkVideoSessionParametersKHR session_params;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), NULL);
  g_return_val_if_fail (params != NULL, NULL);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->session.session)
    return NULL;

  /* *INDENT-OFF* */
  session_params_info = (VkVideoSessionParametersCreateInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,
    .pNext = params,
    .videoSession = priv->session.session->handle,
  };
  /* *INDENT-ON* */

  res = priv->vk.CreateVideoSessionParameters (self->queue->device->device,
      &session_params_info, NULL, &session_params);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkCreateVideoSessionParametersKHR") != VK_SUCCESS)
    return NULL;

  return gst_vulkan_handle_new_wrapped (self->queue->device,
      GST_VULKAN_HANDLE_TYPE_VIDEO_SESSION_PARAMETERS,
      (GstVulkanHandleTypedef) session_params,
      gst_vulkan_handle_free_video_session_parameters,
      priv->vk.DestroyVideoSessionParameters);
}

/**
 * gst_vulkan_encode_picture_new:
 * @self: the #GstVulkanEncoder with the pool's configuration.
 * @in_buffer: (transfer none): the input #GstBuffer.
 * @width: the picture width
 * @height: the picture height
 * @is_ref: the picture reference flag
 * @nb_refs: the picture number of references
 *
 * Create a new vulkan encode picture from the input buffer.
 *
 * Returns: a new #GstVulkanEncodePicture.
 *
 */
GstVulkanEncodePicture *
gst_vulkan_encode_picture_new (GstVulkanEncoder * self, GstBuffer * in_buffer,
    int width, int height, gboolean is_ref, gint nb_refs)
{
  GstVulkanEncodePicture *pic;
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), NULL);
  g_return_val_if_fail (GST_IS_BUFFER (in_buffer), NULL);

  priv = gst_vulkan_encoder_get_instance_private (self);

  pic = g_new0 (GstVulkanEncodePicture, 1);

  if (priv->layered_dpb) {
    g_assert (priv->layered_buffer);
    pic->dpb_buffer = gst_buffer_ref (priv->layered_buffer);
  } else {
    GstFlowReturn ret;

    g_assert (GST_IS_BUFFER_POOL (priv->dpb_pool));
    ret =
        gst_buffer_pool_acquire_buffer (priv->dpb_pool, &pic->dpb_buffer, NULL);
    if (ret != GST_FLOW_OK) {
      gst_vulkan_encode_picture_free (pic);
      return NULL;
    }
  }
  pic->in_buffer = gst_buffer_ref (in_buffer);
  pic->out_buffer =
      gst_vulkan_video_codec_buffer_new (self->queue->device, &priv->profile,
      VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR, priv->out_buffer_size_aligned);
  pic->width = width;
  pic->height = height;
  pic->is_ref = is_ref;
  pic->nb_refs = nb_refs;
  pic->packed_headers =
      g_ptr_array_new_with_free_func ((GDestroyNotify) gst_buffer_unref);
  pic->slotIndex = -1;

  pic->img_view = gst_vulkan_video_image_create_view (pic->in_buffer,
      priv->layered_dpb, TRUE, NULL);

  if (priv->layered_dpb) {
    pic->dpb_view = gst_vulkan_image_view_ref (priv->layered_view);
  } else {
    pic->dpb_view = gst_vulkan_video_image_create_view (pic->dpb_buffer,
        priv->layered_dpb, FALSE, NULL);
  }

  return pic;
}

/**
 * gst_vulkan_encode_picture_free:
 * @pic: the #GstVulkanEncodePicture to free.
 *
 * Free the #GstVulkanEncodePicture.
 *
 */
void
gst_vulkan_encode_picture_free (GstVulkanEncodePicture * pic)
{
  g_return_if_fail (pic != NULL);

  gst_clear_buffer (&pic->in_buffer);
  gst_clear_buffer (&pic->dpb_buffer);
  gst_clear_buffer (&pic->out_buffer);

  gst_vulkan_image_view_unref (pic->img_view);
  pic->img_view = NULL;

  gst_vulkan_image_view_unref (pic->dpb_view);
  pic->dpb_view = NULL;

  g_clear_pointer (&pic->packed_headers, g_ptr_array_unref);

  g_free (pic);
}

/**
 * gst_vulkan_encoder_caps:
 * @self: a #GstVulkanEncoder
 * @caps: (out): a #GstVulkanVideoCapabilities
 *
 * Get the #GstVulkanVideoCapabilities of the encoder if available
 *
 * Returns: whether the encoder has vulkan encoder caps.
 *
 */
gboolean
gst_vulkan_encoder_caps (GstVulkanEncoder * self,
    GstVulkanVideoCapabilities * caps)
{
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->started)
    return FALSE;

  if (caps) {
    *caps = priv->caps;
    caps->caps.pNext = &caps->codec;
  }

  return TRUE;
}

/**
 * gst_vulkan_encoder_profile_caps:
 * @self: a #GstVulkanEncoder
 *
 * Get the #GstCaps according to the encoder video profile
 *
 * Returns: (transfer full): #GstCaps of the profile defined at gst_vulkan_encoder_start()
 *
 */
GstCaps *
gst_vulkan_encoder_profile_caps (GstVulkanEncoder * self)
{
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), NULL);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->started)
    return NULL;

  return gst_caps_ref (priv->profile_caps);
}

/**
 * gst_vulkan_encoder_stop:
 * @self: a #GstVulkanEncoder
 *
 * Stop the encoder.
 *
 * Returns: whether the encoder stopped correctly.
 *
 */
gboolean
gst_vulkan_encoder_stop (GstVulkanEncoder * self)
{
  GstVulkanEncoderPrivate *priv;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);
  if (!priv->started)
    return TRUE;

  gst_vulkan_video_session_destroy (&priv->session);

  gst_clear_caps (&priv->profile_caps);

  gst_clear_vulkan_handle (&priv->session_params);

  if (priv->layered_view)
    gst_vulkan_image_view_unref (priv->layered_view);
  gst_clear_buffer (&priv->layered_buffer);
  gst_clear_object (&priv->dpb_pool);

  gst_clear_object (&priv->exec);

  priv->started = FALSE;

  return TRUE;
}

/**
 * gst_vulkan_encoder_start:
 * @self: a #GstVulkanEncoder
 * @profile: a #GstVulkanVideoProfile
 * @out_buffer_size: a maximal buffer size to be used by the encoder to store the output
 * @error: (out) : an error result in case of failure or %NULL
 *
 * Start the encoding session according to a valid Vulkan profile
 *
 * Returns: whether the encoder started correctly.
 *
 */
gboolean
gst_vulkan_encoder_start (GstVulkanEncoder * self,
    GstVulkanVideoProfile * profile, guint out_buffer_size, GError ** error)
{
  GstVulkanEncoderPrivate *priv;
  VkResult res;
  VkVideoSessionCreateInfoKHR session_create;
  VkPhysicalDevice gpu;
  VkFormat pic_format = VK_FORMAT_UNDEFINED;
  int codec_idx;
  GstVulkanCommandPool *cmd_pool;
  VkQueryPoolVideoEncodeFeedbackCreateInfoKHR query_create;
  GError *query_err = NULL;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);
  g_return_val_if_fail (profile != NULL, FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (priv->started)
    return TRUE;

  if (!_populate_function_table (self)) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "Couldn't load Vulkan Video functions");
    return FALSE;
  }

  switch (self->codec) {
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
      if (!gst_vulkan_video_profile_is_valid (profile, self->codec)) {
        g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
            "Invalid profile");
        return FALSE;
      }
      priv->caps.codec.h264enc = (VkVideoEncodeH264CapabilitiesKHR) {
        /* *INDENT-OFF* */
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR,
        /* *INDENT-ON* */
      };
      codec_idx = GST_VK_VIDEO_EXTENSION_ENCODE_H264;
      break;
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
      if (!gst_vulkan_video_profile_is_valid (profile, self->codec)) {
        g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
            "Invalid profile");
        return FALSE;
      }
      priv->caps.codec.h265enc = (VkVideoEncodeH265CapabilitiesKHR) {
        /* *INDENT-OFF* */
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR,
        /* *INDENT-ON* */
      };
      codec_idx = GST_VK_VIDEO_EXTENSION_ENCODE_H265;
      break;
    default:
      g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
          "Invalid codec");
      return FALSE;
  }

  priv->profile = *profile;

  /* ensure the chain up of structure */
  priv->profile.usage.encode.pNext = &priv->profile.codec;
  priv->profile.profile.pNext = &priv->profile.usage.encode;

  /* *INDENT-OFF* */
  priv->enc_caps = (VkVideoEncodeCapabilitiesKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,
    .pNext = &priv->caps.codec,
  };
  priv->caps.caps = (VkVideoCapabilitiesKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,
    .pNext = &priv->enc_caps,
  };
  /* *INDENT-ON* */

  gpu = gst_vulkan_device_get_physical_device (self->queue->device);
  res = priv->vk.GetPhysicalDeviceVideoCapabilities (gpu,
      &priv->profile.profile, &priv->caps.caps);
  if (gst_vulkan_error_to_g_error (res, error,
          "vkGetPhysicalDeviceVideoCapabilitiesKHR") != VK_SUCCESS)
    return FALSE;

  if (_vk_codec_extensions[codec_idx].specVersion <
      _vk_codec_supported_extensions[codec_idx]) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "STD version headers [%i.%i.%i] not supported, need at least [%i.%i.%i], check your SDK path.",
        VK_CODEC_VERSION (_vk_codec_extensions[codec_idx].specVersion),
        VK_CODEC_VERSION (_vk_codec_supported_extensions[codec_idx]));
    return FALSE;
  }

  if (_vk_codec_extensions[codec_idx].specVersion <
      priv->caps.caps.stdHeaderVersion.specVersion) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "The driver needs a newer version [%i.%i.%i] of the current headers %d.%d.%d, please update the code to support this driver.",
        VK_CODEC_VERSION (priv->caps.caps.stdHeaderVersion.specVersion),
        VK_CODEC_VERSION (_vk_codec_extensions[codec_idx].specVersion));
    return FALSE;
  }

  GST_OBJECT_LOCK (self);
  if ((priv->prop.rate_control != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR)
      && !(priv->prop.rate_control & priv->enc_caps.rateControlModes)) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "The driver does not support the rate control requested %d, driver caps: %d",
        priv->prop.rate_control, priv->enc_caps.rateControlModes);
    GST_OBJECT_UNLOCK (self);
    return FALSE;
  }

  if (priv->enc_caps.maxQualityLevels
      && priv->prop.quality_level >= priv->enc_caps.maxQualityLevels) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "The driver does not support the quality level requested %d, driver caps: %d",
        priv->prop.quality_level, priv->enc_caps.maxQualityLevels);
    GST_OBJECT_UNLOCK (self);
    return FALSE;
  }

  if (priv->enc_caps.maxBitrate
      && priv->prop.average_bitrate >= priv->enc_caps.maxBitrate) {
    g_set_error (error, GST_VULKAN_ERROR, VK_ERROR_INITIALIZATION_FAILED,
        "The driver does not support the average bitrate requested %d, driver caps: %"
        G_GUINT64_FORMAT, priv->prop.average_bitrate,
        priv->enc_caps.maxBitrate);
    GST_OBJECT_UNLOCK (self);
    return FALSE;
  }
  GST_OBJECT_UNLOCK (self);

  /* Get output format */
  pic_format = gst_vulkan_video_encoder_get_format (self,
      VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR |
      VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, error);
  if (pic_format == VK_FORMAT_UNDEFINED)
    return FALSE;

  cmd_pool = gst_vulkan_queue_create_command_pool (self->queue, error);
  if (!cmd_pool)
    return FALSE;
  priv->exec = gst_vulkan_operation_new (cmd_pool);
  gst_object_unref (cmd_pool);

  /* we don't want overridden parameters in queries */
  /* *INDENT-OFF* */
  query_create = (VkQueryPoolVideoEncodeFeedbackCreateInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,
    .pNext = &profile->profile,
    .encodeFeedbackFlags = priv->enc_caps.supportedEncodeFeedbackFlags &
        (~VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_HAS_OVERRIDES_BIT_KHR),
  };
  /* *INDENT-ON* */

  if (!gst_vulkan_operation_enable_query (priv->exec,
          VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR, 1, &query_create,
          &query_err)) {
    if (query_err->code != VK_ERROR_FEATURE_NOT_PRESENT) {
      g_propagate_error (error, query_err);
      goto failed;
    }
    g_clear_error (&query_err);
  }

  priv->profile_caps = gst_vulkan_video_profile_to_caps (&priv->profile);

  GST_LOG_OBJECT (self, "Capabilities for %" GST_PTR_FORMAT ":\n"
      "     Width from %i to %i\n"
      "     Height from %i to %i\n"
      "     MaxBitrate: %" G_GUINT64_FORMAT "\n"
      "     Encode mode:%s",
      priv->profile_caps,
      priv->caps.caps.minCodedExtent.width,
      priv->caps.caps.maxCodedExtent.width,
      priv->caps.caps.minCodedExtent.height,
      priv->caps.caps.maxCodedExtent.height,
      priv->enc_caps.maxBitrate,
      priv->caps.caps.flags &
      VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR ?
      " separate_references" : "");

  priv->layered_dpb =
      !(priv->caps.
      caps.flags & VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR);

  priv->caps.caps.pNext = NULL;

  /* *INDENT-OFF* */
  session_create = (VkVideoSessionCreateInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,
    .queueFamilyIndex = self->queue->family,
    .pVideoProfile = &profile->profile,
    .pictureFormat = pic_format,
    .maxCodedExtent = priv->caps.caps.maxCodedExtent,
    .referencePictureFormat = pic_format,
    .maxDpbSlots = priv->caps.caps.maxDpbSlots,
    .maxActiveReferencePictures = priv->caps.caps.maxActiveReferencePictures,
    .pStdHeaderVersion = &_vk_codec_extensions[codec_idx],
  };
  /* *INDENT-ON* */

  if (!gst_vulkan_video_session_create (&priv->session, self->queue->device,
          &priv->vk, &session_create, error))
    goto failed;


  priv->out_buffer_size_aligned = GST_ROUND_UP_N (out_buffer_size,
      priv->caps.caps.minBitstreamBufferSizeAlignment);

  priv->started = TRUE;

  return TRUE;

failed:
  gst_clear_object (&priv->exec);
  gst_clear_caps (&priv->profile_caps);
  return FALSE;
}

/**
 * gst_vulkan_encoder_update_video_session_parameters:
 * @self: a #GstVulkanEncoder
 * @params: a #GstVulkanEncoderParameters
 * @error: (out) (optional): an error result in case of failure
 *
 * Set the sessions parameters to be used by the encoder
 *
 * Returns: whether the encoder updated the session parameters correctly.
 *
 */
gboolean
gst_vulkan_encoder_update_video_session_parameters (GstVulkanEncoder * self,
    GstVulkanEncoderParameters * params, GError ** error)
{
  GstVulkanEncoderPrivate *priv;
  GstVulkanHandle *handle;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);
  g_return_val_if_fail (params != NULL, FALSE);

  handle =
      gst_vulkan_encoder_new_video_session_parameters (self, params, error);
  if (!handle)
    return FALSE;

  priv = gst_vulkan_encoder_get_instance_private (self);

  gst_clear_vulkan_handle (&priv->session_params);
  priv->session_params = handle;

  return TRUE;
}

/**
 * gst_vulkan_encoder_video_session_parameters_overrides:
 * @self: a #GstVulkanEncoder
 * @params: a #GstVulkanEncoderParametersOverrides
 * @feedback: (out) (optional): #GstVulkanEncoderParametersFeedback or %NULL
 * @data_size: (out) (optional): the allocated size of @data
 * @data: (out) (optional): location to store the requested overrides, use
 *     g_free() to free after use
 *
 * 42.15.1. Codec-Specific Semantics
 *
 * Implementations supporting video encode operations for any particular video
 * codec operation often support only a subset of the available encoding tools
 * defined by the corresponding video compression standards.
 *
 * … this specification allows implementations to override the value of any of
 * the codec-specific parameters,
 *
 * Returns: whether the encoder has bew sessions parameters.
 *
 */
gboolean
gst_vulkan_encoder_video_session_parameters_overrides (GstVulkanEncoder * self,
    GstVulkanEncoderParametersOverrides * params,
    GstVulkanEncoderParametersFeedback * feedback, gsize * data_size,
    gpointer * data, GError ** error)
{
  VkVideoEncodeSessionParametersGetInfoKHR video_params_info;
  VkVideoEncodeSessionParametersFeedbackInfoKHR feedback_info;
  VkResult res;
  GstVulkanEncoderPrivate *priv;
  gsize size;
  gpointer param_data;
  gboolean write;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);
  g_return_val_if_fail (params != NULL && feedback != NULL, FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);
  if (!priv->started)
    return FALSE;

  switch (self->codec) {
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
      if (params->h264.sType !=
          VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR) {
        gst_vulkan_error_to_g_error (GST_VULKAN_ERROR, error,
            "Invalid parameter for H.264");
        return FALSE;
      }
      write = params->h264.writeStdPPS || params->h264.writeStdSPS;
      if (feedback) {
        feedback->h264.sType =
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
      }
      break;
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
      if (params->h265.sType !=
          VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR) {
        gst_vulkan_error_to_g_error (GST_VULKAN_ERROR, error,
            "Invalid parameter for H.265");
        return FALSE;
      }
      write = params->h265.writeStdPPS || params->h265.writeStdSPS
          || params->h265.writeStdVPS;
      if (feedback) {
        feedback->h265.sType =
            VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR;
      }
      break;
    default:
      return FALSE;
  }

  /* *INDENT-OFF* */
  video_params_info = (VkVideoEncodeSessionParametersGetInfoKHR) {
	  .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
		.pNext = params,
		.videoSessionParameters = priv->session_params->handle,
  };

  feedback_info = (VkVideoEncodeSessionParametersFeedbackInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,
    .pNext = feedback,
  };
  /* *INDENT-ON* */

  res = priv->vk.GetEncodedVideoSessionParameters (self->queue->device->device,
      &video_params_info, &feedback_info, &size, NULL);
  if (gst_vulkan_error_to_g_error (res, error,
          "vGetEncodedVideoSessionParametersKHR") != VK_SUCCESS)
    return FALSE;

  /* FIXME: forcing because a bug in NVIDIA driver */
  feedback_info.hasOverrides = 1;
  if (!feedback_info.hasOverrides || !data || !write)
    return TRUE;

  GST_DEBUG_OBJECT (self, "allocating for bitstream parameters %"
      G_GSIZE_FORMAT, size);
  param_data = g_malloc (size);

  res = priv->vk.GetEncodedVideoSessionParameters (self->queue->device->device,
      &video_params_info, &feedback_info, &size, param_data);
  if (gst_vulkan_error_to_g_error (res, error,
          "vGetEncodedVideoSessionParametersKHR") != VK_SUCCESS)
    return FALSE;

  if (data_size)
    *data_size = size;
  *data = param_data;

  return TRUE;
}

/**
 * gst_vulkan_encoder_create_dpb_pool:
 * @self: a #GstVulkanEncoder
 * @caps: the #GstCaps of the DPB pool
 *
 * Instantiates an internal Vulkan image pool for driver encoders whose output
 * buffers can be used as DPB buffers.
 *
 * Returns: whether the pool was created.
 */
gboolean
gst_vulkan_encoder_create_dpb_pool (GstVulkanEncoder * self, GstCaps * caps)
{
  GstVulkanEncoderPrivate *priv;
  GstCaps *profile_caps;
  GstStructure *config;
  guint min_buffers, max_buffers;
  GstFlowReturn ret;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);
  g_return_val_if_fail (GST_IS_CAPS (caps), FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);

  if (!priv->started)
    return FALSE;

  if ((!priv->layered_dpb && priv->dpb_pool)
      || (priv->layered_dpb && priv->layered_buffer))
    return TRUE;

  if (priv->layered_dpb) {
    min_buffers = max_buffers = 1;
  } else {
    min_buffers = priv->caps.caps.maxDpbSlots;
    max_buffers = 0;
  }

  priv->dpb_pool = gst_vulkan_image_buffer_pool_new (self->queue->device);

  config = gst_buffer_pool_get_config (priv->dpb_pool);
  gst_buffer_pool_config_set_params (config, caps, 1024, min_buffers,
      max_buffers);
  gst_vulkan_image_buffer_pool_config_set_allocation_params (config,
      VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR,
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

  if (priv->layered_dpb) {
    gst_structure_set (config, "num-layers", G_TYPE_UINT,
        priv->caps.caps.maxDpbSlots, NULL);
  }
  profile_caps = gst_vulkan_encoder_profile_caps (self);
  gst_vulkan_image_buffer_pool_config_set_encode_caps (config, profile_caps);
  gst_caps_unref (profile_caps);

  if (!gst_buffer_pool_set_config (priv->dpb_pool, config))
    goto bail;
  if (!gst_buffer_pool_set_active (priv->dpb_pool, TRUE))
    goto bail;

  if (priv->layered_dpb) {
    ret = gst_buffer_pool_acquire_buffer (priv->dpb_pool, &priv->layered_buffer,
        NULL);
    if (ret != GST_FLOW_OK)
      goto bail;

    priv->layered_view =
        gst_vulkan_video_image_create_view (priv->layered_buffer,
        priv->layered_dpb, FALSE, NULL);

    gst_clear_object (&priv->dpb_pool);
  }

  return TRUE;

bail:
  gst_clear_object (&priv->dpb_pool);
  return FALSE;
}

/**
 * gst_vulkan_encoder_encode:
 * @self: a #GstVulkanEncoder
 * @pic: a #GstVulkanEncodePicture
 * @ref_pics: an array of #GstVulkanEncodePicture
 *
 * Encode a picture according to its reference pictures.
 *
 * Returns: whether the encode process completed successfully.
 *
 */
gboolean
gst_vulkan_encoder_encode (GstVulkanEncoder * self,
    GstVulkanEncodePicture * pic, GstVulkanEncodePicture ** ref_pics)
{
  GstVulkanEncoderPrivate *priv;
  GError *err = NULL;
  gboolean ret = TRUE;
  GstMemory *mem;
  int i;
  GstVulkanEncodeQueryResult *encode_res;
  guint n_mems = 0;
  gsize params_size = 0;
  VkVideoEncodeRateControlLayerInfoKHR rate_control_layer;
  VkVideoEncodeQualityLevelInfoKHR quality_level_info;
  VkVideoCodingControlInfoKHR coding_ctrl;
  VkVideoBeginCodingInfoKHR begin_coding;
  VkVideoEncodeInfoKHR encode_info;
  VkVideoEndCodingInfoKHR end_coding;
  gint maxDpbSlots;
  VkVideoReferenceSlotInfoKHR ref_slots[37];
  gint ref_slot_num = 0;
  GstVulkanCommandBuffer *cmd_buf;
  GArray *barriers;

  g_return_val_if_fail (GST_IS_VULKAN_ENCODER (self), FALSE);
  g_return_val_if_fail (pic != NULL, FALSE);

  priv = gst_vulkan_encoder_get_instance_private (self);

  maxDpbSlots = priv->layered_dpb ? 2 : priv->caps.caps.maxDpbSlots;

  /* initialize the vulkan operation */
  if (!gst_vulkan_operation_begin (priv->exec, &err))
    goto bail;

  /* Prepare the encoding scope by flling the VkVideoBeginCodingInfoKHR structure */
  /* *INDENT-OFF* */
  begin_coding = (VkVideoBeginCodingInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,
    .pNext = NULL,
    .videoSession = priv->session.session->handle,
    .videoSessionParameters = priv->session_params->handle,
  };
  coding_ctrl = (VkVideoCodingControlInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,
  };
  /* *INDENT-ON* */

  /* First run, some information such as rate_control and slot index must be initialized. */
  if (!priv->first_encode_cmd) {
    priv->current_slot_index = 0;
    GST_OBJECT_LOCK (self);
    /* *INDENT-OFF* */
    rate_control_layer = (VkVideoEncodeRateControlLayerInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,
      .pNext = pic->codec_rc_layer_info,
      .averageBitrate = priv->prop.average_bitrate,
      .maxBitrate = priv->enc_caps.maxBitrate,
      .frameRateNumerator = pic->fps_n,
      .frameRateDenominator = pic->fps_d,
    };
    priv->rate_control_info = (VkVideoEncodeRateControlInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,
      .rateControlMode = priv->prop.rate_control,
      .layerCount = 0,
      .pLayers = NULL,
      .initialVirtualBufferSizeInMs = 0,
      .virtualBufferSizeInMs = 0,
    };
    /* *INDENT-ON* */
    switch (priv->prop.rate_control) {
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR:
        begin_coding.pNext = &priv->rate_control_info;
        break;
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR:
        rate_control_layer.maxBitrate = rate_control_layer.averageBitrate;
        begin_coding.pNext = &priv->rate_control_info;
        break;
      case VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR:
        priv->rate_control_info.layerCount = 1;
        priv->rate_control_info.pLayers = &rate_control_layer;
        priv->rate_control_info.virtualBufferSizeInMs = 1;
        begin_coding.pNext = &priv->rate_control_info;
        break;
    };
    GST_OBJECT_UNLOCK (self);
  }

  g_assert (pic->dpb_buffer && pic->dpb_view);
  g_assert (pic->in_buffer && pic->img_view);
  g_assert (pic->out_buffer);

  /* Set the ref slots according to the pic refs to bound the video
     session encoding. It should contain all the references + 1 to book
     a new slotIndex (-1) for the current picture. */
  /* *INDENT-OFF* */
  pic->dpb = (VkVideoPictureResourceInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
    .pNext = NULL,
    .codedOffset = { 0, 0 },
    .codedExtent = {
      .width = pic->width,
      .height = pic->height
    },
    .baseArrayLayer = 0,
    .imageViewBinding = pic->dpb_view->view,
  };
  /* *INDENT-ON* */

  for (i = 0; i < pic->nb_refs; i++) {
    /* *INDENT-OFF* */
    ref_slots[i] = (VkVideoReferenceSlotInfoKHR) {
      .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
      .pNext = ref_pics[i]->codec_dpb_slot_info,
      .slotIndex = ref_pics[i]->slotIndex,
      .pPictureResource = &ref_pics[i]->dpb,
    };
    /* *INDENT-ON* */
    ref_slot_num++;
  }

  /* *INDENT-OFF* */
  ref_slots[ref_slot_num] = (VkVideoReferenceSlotInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,
    .pNext = pic->codec_dpb_slot_info,
    .slotIndex = pic->slotIndex,
    .pPictureResource = &pic->dpb,
  };
  /* *INDENT-ON* */

  ref_slot_num++;

  /* Setup the begin coding structure using the reference slots */
  begin_coding.referenceSlotCount = ref_slot_num;
  begin_coding.pReferenceSlots = ref_slots;

  cmd_buf = priv->exec->cmd_buf;
  priv->vk.CmdBeginVideoCoding (cmd_buf->cmd, &begin_coding);

  /* 42.9. Video Coding Control
     To apply dynamic controls to the currently bound video session object such as
     quality information. This should be done when requesting a new coding contol ie
     first attempt of encoding.
   */
  if (!priv->first_encode_cmd) {
    coding_ctrl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    coding_ctrl.pNext = NULL;
    priv->vk.CmdControlVideoCoding (cmd_buf->cmd, &coding_ctrl);

    if (priv->prop.quality_level
        && priv->prop.quality_level <= priv->enc_caps.maxQualityLevels) {

      /* *INDENT-OFF* */
      quality_level_info = (VkVideoEncodeQualityLevelInfoKHR) {
        .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,
        .qualityLevel = priv->prop.quality_level,
      };
      /* *INDENT-ON* */

      coding_ctrl.pNext = &quality_level_info;
      coding_ctrl.flags = VK_VIDEO_CODING_CONTROL_ENCODE_QUALITY_LEVEL_BIT_KHR;
      GST_INFO ("quality_level_info.qualityLevel %d",
          quality_level_info.qualityLevel);
      priv->vk.CmdControlVideoCoding (cmd_buf->cmd, &coding_ctrl);
    }

    if (priv->prop.rate_control !=
        VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR) {

      coding_ctrl.pNext = &priv->rate_control_info;
      coding_ctrl.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
      GST_INFO ("rate_control_info.rateControlMode %d",
          priv->rate_control_info.rateControlMode);
      priv->vk.CmdControlVideoCoding (cmd_buf->cmd, &coding_ctrl);
    }
    priv->first_encode_cmd = TRUE;
  }

  /* Add the packed headers if present on head of the output buffer */
  for (i = 0; pic->packed_headers && i < pic->packed_headers->len; i++) {
    GstBuffer *buffer;
    GstMapInfo info;
    buffer = g_ptr_array_index (pic->packed_headers, i);
    gst_buffer_map (buffer, &info, GST_MAP_READ);
    GST_MEMDUMP ("params buffer", info.data, info.size);
    gst_buffer_unmap (buffer, &info);
    params_size += gst_buffer_get_size (buffer);
    mem = gst_memory_copy (gst_buffer_peek_memory (buffer, 0), 0, -1);
    gst_buffer_insert_memory (pic->out_buffer, i, mem);
    n_mems++;
  }
  g_clear_pointer (&pic->packed_headers, g_ptr_array_unref);
  /* Peek the output memory to be used by VkVideoEncodeInfoKHR.dstBuffer */
  mem = gst_buffer_peek_memory (pic->out_buffer, n_mems);

  /* Attribute a free slot index to the picture to be used later as a reference.
   * The picture is kept until it remains useful to the encoding process.*/
  pic->slotIndex = priv->current_slot_index;
  ref_slots[ref_slot_num - 1].slotIndex = pic->slotIndex;
  priv->current_slot_index++;
  if (priv->current_slot_index >= maxDpbSlots)
    priv->current_slot_index = 0;

  /* Setup the encode info */
  /* *INDENT-OFF* */
  encode_info = (VkVideoEncodeInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,
    .pNext = pic->codec_pic_info,
    .flags = 0x0,
    .dstBuffer = ((GstVulkanBufferMemory *) mem)->buffer,
    .dstBufferOffset = 0,
    .dstBufferRange = gst_memory_get_sizes (mem, NULL, NULL),
    .srcPictureResource = (VkVideoPictureResourceInfoKHR) { // SPEC: this should be separate
        .sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,
        .pNext = NULL,
        .codedOffset = { 0, 0 },
        .codedExtent = {
          .width = pic->width,
          .height = pic->height
        },
        .baseArrayLayer = 0,
        .imageViewBinding = pic->img_view->view,
    },
    .pSetupReferenceSlot = &ref_slots[ref_slot_num - 1],
    .referenceSlotCount = pic->nb_refs,
    .pReferenceSlots = pic->nb_refs ? ref_slots  : NULL,
    .precedingExternallyEncodedBytes = 0,
  };
  /* *INDENT-ON* */

  gst_vulkan_operation_add_dependency_frame (priv->exec, pic->in_buffer,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR);
  gst_vulkan_operation_add_frame_barrier (priv->exec, pic->in_buffer,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
      VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
      VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR, NULL);

  gst_vulkan_operation_add_dependency_frame (priv->exec, pic->dpb_buffer,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR);
  gst_vulkan_operation_add_frame_barrier (priv->exec, pic->dpb_buffer,
      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR,
      VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR,
      VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR, NULL);

  barriers = gst_vulkan_operation_retrieve_image_barriers (priv->exec);

  /* *INDENT-OFF* */
  vkCmdPipelineBarrier2 (cmd_buf->cmd, &(VkDependencyInfo) {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
      .pImageMemoryBarriers = (VkImageMemoryBarrier2 *) barriers->data,
      .imageMemoryBarrierCount = barriers->len,
      }
  );
  /* *INDENT-ON* */
  g_array_unref (barriers);

  gst_vulkan_operation_begin_query (priv->exec, 0);
  priv->vk.CmdEncodeVideo (cmd_buf->cmd, &encode_info);
  gst_vulkan_operation_end_query (priv->exec, 0);

  /* *INDENT-OFF* */
  end_coding = (VkVideoEndCodingInfoKHR) {
    .sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,
  };
  /* *INDENT-ON* */

  /* 41.5 4. vkCmdEndVideoCodingKHR signals the end of the recording of the
   * Vulkan Video Context, as established by vkCmdBeginVideoCodingKHR. */
  priv->vk.CmdEndVideoCoding (cmd_buf->cmd, &end_coding);

  if (!gst_vulkan_operation_end (priv->exec, &err)) {
    GST_ERROR_OBJECT (self, "The operation did not complete properly");
    goto bail;
  }
  /* Wait the operation to complete or we might have a failing query */
  gst_vulkan_operation_wait (priv->exec);

  gst_vulkan_operation_get_query (priv->exec, (gpointer *) & encode_res, &err);
  if (encode_res->status == VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
    GST_INFO_OBJECT (self, "The frame %d has been encoded with size %lu",
        pic->pic_num, encode_res->data_size + params_size);
    gst_buffer_resize (pic->out_buffer, encode_res->offset,
        encode_res->data_size + params_size);
  } else {
    GST_ERROR_OBJECT (self,
        "The operation did not complete properly, query status = %d",
        encode_res->status);
    goto bail;
  }

  return ret;
bail:
  {
    return FALSE;
  }
}

/**
 * gst_vulkan_create_encoder_from_queue:
 * @queue: a #GstVulkanQueue
 * @codec: (type guint): the VkVideoCodecOperationFlagBitsKHR to encode
 *
 * Creates a #GstVulkanEncoder object if @codec encoding is supported by @queue
 *
 * Returns: (transfer full) (nullable): the #GstVulkanEncoder object
 *
 */
GstVulkanEncoder *
gst_vulkan_encoder_create_from_queue (GstVulkanQueue * queue, guint codec)
{
  GstVulkanPhysicalDevice *device;
  GstVulkanEncoder *encoder;
  guint flags, expected_flag, supported_video_ops;
  const char *extension;

  g_return_val_if_fail (GST_IS_VULKAN_QUEUE (queue), NULL);

  device = queue->device->physical_device;
  expected_flag = VK_QUEUE_VIDEO_ENCODE_BIT_KHR;
  flags = device->queue_family_props[queue->family].queueFlags;
  supported_video_ops = device->queue_family_ops[queue->family].video;

  if (device->properties.apiVersion < VK_MAKE_VERSION (1, 3, 275)) {
    GST_WARNING_OBJECT (queue,
        "API version %d.%d.%d doesn't support video encode extensions",
        VK_VERSION_MAJOR (device->properties.apiVersion),
        VK_VERSION_MINOR (device->properties.apiVersion),
        VK_VERSION_PATCH (device->properties.apiVersion));
    return NULL;
  }

  switch (codec) {
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR:
      extension = VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME;
      break;
    case VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR:
      extension = VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME;
      break;
    default:
      GST_WARNING_OBJECT (queue, "Unsupported codec");
      return NULL;
  }

  if ((flags & expected_flag) != expected_flag) {
    GST_WARNING_OBJECT (queue, "Queue doesn't support encoding");
    return NULL;
  }
  if ((supported_video_ops & codec) != codec) {
    GST_WARNING_OBJECT (queue, "Queue doesn't support codec encoding");
    return NULL;
  }

  if (!(gst_vulkan_device_is_extension_enabled (queue->device,
              VK_KHR_VIDEO_QUEUE_EXTENSION_NAME)
          && gst_vulkan_device_is_extension_enabled (queue->device,
              VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME)
          && gst_vulkan_device_is_extension_enabled (queue->device, extension)))
    return NULL;

  encoder = g_object_new (GST_TYPE_VULKAN_ENCODER, NULL);
  gst_object_ref_sink (encoder);
  encoder->queue = gst_object_ref (queue);
  encoder->codec = codec;

  return encoder;
}
