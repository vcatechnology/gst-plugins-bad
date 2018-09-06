/* GStreamer
 *
 *  Copyright 2018 VCA Technology Ltd.
 *   @author: Joel Holdsworth <joel.holdsworth@vcatechnology.com>
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
/**
 * SECTION:element-autoconvert2
 * @title: autoconvert2
 * @short_description: Constructs a graph of elements based on the caps.
 *
 * The #autovideoconvert2 is a specialized version of the #autoconvert2 element
 * that is designed for converting video imagery.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstautovideoconvert2.h"

GST_DEBUG_CATEGORY (autovideoconvert2_debug);
#define GST_CAT_DEFAULT (autovideoconvert2_debug)

struct Size
{
  gint width, height;
};

struct _GstAutoVideoConvert2Priv
{
  struct Size min_sink_image_size, max_sink_image_size;
};

static void gst_auto_video_convert2_finalize (GObject * object);

static GList *gst_auto_video_convert2_get_factories (GstAutoConvert2 *
    autoconvert2);
static gboolean
gst_auto_video_convert2_validate_transform_route (GstAutoConvert2 *
    autoconvert2, const GstAutoConvert2TransformRoute * route);
static guint gst_auto_video_convert2_cost_transformation_step (GstAutoConvert2 *
    autoconvert2, const GstAutoConvert2TransformationStep *
    transformation_step);
static void gst_auto_video_convert2_begin_building_graph (GstAutoConvert2 *
    autoconvert2);

static gboolean element_filter (GstPluginFeature * feature,
    GstAutoVideoConvert2 * autovideoconvert2);
static GList *create_factory_list (GstAutoVideoConvert2 * autovideoconvert2);
static void update_factory_list (GstAutoVideoConvert2 * autovideoconvert2);

static gboolean get_caps_image_size (GstCaps * caps, struct Size *size);
static gboolean get_caps_frame_rate (GstCaps * caps, gint * numerator,
    gint * denominator);

static GMutex factories_mutex;
static guint32 factories_cookie = 0;    /* Cookie from last time when factories was updated */
static GList *factories = NULL; /* factories we can use for selecting elements */

G_DEFINE_TYPE (GstAutoVideoConvert2, gst_auto_video_convert2,
    GST_TYPE_AUTO_CONVERT2);

static void
gst_auto_video_convert2_class_init (GstAutoVideoConvert2Class * klass)
{
  GObjectClass *const gobject_class = (GObjectClass *) klass;
  GstAutoConvert2Class *const gstautoconvert2_class =
      (GstAutoConvert2Class *) klass;

  GST_DEBUG_CATEGORY_INIT (autovideoconvert2_debug, "autovideoconvert2", 0,
      "autovideoconvert2 element");

  gstautoconvert2_class->get_factories =
      GST_DEBUG_FUNCPTR (gst_auto_video_convert2_get_factories);
  gstautoconvert2_class->validate_transform_route =
      GST_DEBUG_FUNCPTR (gst_auto_video_convert2_validate_transform_route);
  gstautoconvert2_class->cost_transformation_step =
      GST_DEBUG_FUNCPTR (gst_auto_video_convert2_cost_transformation_step);
  gstautoconvert2_class->begin_building_graph =
      GST_DEBUG_FUNCPTR (gst_auto_video_convert2_begin_building_graph);

  gobject_class->finalize =
      GST_DEBUG_FUNCPTR (gst_auto_video_convert2_finalize);
}

static void
gst_auto_video_convert2_init (GstAutoVideoConvert2 * autovideoconvert2)
{
  autovideoconvert2->priv = g_malloc0 (sizeof (GstAutoVideoConvert2Priv));
}

static void
gst_auto_video_convert2_finalize (GObject * object)
{
  GstAutoVideoConvert2 *autovideoconvert2 = GST_AUTO_VIDEO_CONVERT2 (object);
  g_free (autovideoconvert2->priv);
}

static GList *
gst_auto_video_convert2_get_factories (GstAutoConvert2 * autoconvert2)
{
  GstAutoVideoConvert2 *autovideoconvert2 =
      (GstAutoVideoConvert2 *) autoconvert2;
  update_factory_list (autovideoconvert2);
  return factories;
}

static gboolean
gst_auto_video_convert2_validate_transform_route (GstAutoConvert2 *
    autoconvert2, const GstAutoConvert2TransformRoute * route)
{
  const GstAutoVideoConvert2 *const autovideoconvert2 =
      (GstAutoVideoConvert2 *) autoconvert2;
  const struct Size max_size = autovideoconvert2->priv->max_sink_image_size;
  struct Size sink_size = { INT_MAX, INT_MAX };
  struct Size src_size = { INT_MIN, INT_MIN };

  if (get_caps_image_size (route->sink.caps, &sink_size) &&
      get_caps_image_size (route->src.caps, &src_size)) {

    if (max_size.width != INT_MIN) {
      /* If we have a larger image, don't enlarge a smaller image. */
      if (max_size.width > src_size.width &&
          max_size.height > src_size.height &&
          (sink_size.width < src_size.width ||
              sink_size.height < src_size.height))
        return FALSE;

      /* When enlarging... */
      if (max_size.width < src_size.width && max_size.height < src_size.height) {
        /* ...never shrink from a larger image. */
        if (src_size.width < sink_size.width
            || src_size.height < sink_size.height)
          return FALSE;

        /* ...don't use any input smaller than the largest input. */
        if (max_size.width > sink_size.width
            || max_size.height > sink_size.height)
          return FALSE;
      }
    }
  }

  return TRUE;
}

static guint
gst_auto_video_convert2_cost_transformation_step (GstAutoConvert2 *
    autoconvert2, const GstAutoConvert2TransformationStep * transformation_step)
{
  struct Size size;
  gint num, den;
  guint cost = 0;

  if (!get_caps_frame_rate (transformation_step->src_caps, &num, &den))
    num = 30, den = 1;
  if (get_caps_image_size (transformation_step->src_caps, &size))
    cost += (size.width * size.height * num) / den;

  if (!get_caps_frame_rate (transformation_step->sink_caps, &num, &den))
    num = 30, den = 1;
  if (get_caps_image_size (transformation_step->sink_caps, &size))
    cost += (size.width * size.height * num) / den;

  return cost ? cost : 1;
}

static void
gst_auto_video_convert2_begin_building_graph (GstAutoConvert2 * autoconvert2)
{
  GstAutoVideoConvert2 *const autovideoconvert2 =
      (GstAutoVideoConvert2 *) autoconvert2;
  struct Size min_size = { INT_MAX, INT_MAX };
  struct Size max_size = { INT_MIN, INT_MIN };
  struct Size size;
  GList *i;

  /* Capture the caps of the current set of sink pads. */
  for (i = ((const GstElement *) autoconvert2)->sinkpads; i; i = i->next) {
    GstPad *const sink_pad = (GstPad *) i->data;
    GstCaps *const sink_caps = gst_pad_get_current_caps (sink_pad);
    if (get_caps_image_size (sink_caps, &size)) {
      if (size.width < min_size.width && size.height < min_size.height)
        min_size = size;
      if (size.width > max_size.width && size.height > max_size.height)
        max_size = size;
    }
    gst_caps_unref (sink_caps);
  }

  autovideoconvert2->priv->min_sink_image_size = min_size;
  autovideoconvert2->priv->max_sink_image_size = max_size;
}

static gboolean
element_filter (GstPluginFeature * feature,
    GstAutoVideoConvert2 * autovideoconvert2)
{
  const gchar *klass;

  /* We only care about element factories */
  if (G_UNLIKELY (!GST_IS_ELEMENT_FACTORY (feature)))
    return FALSE;

  klass = gst_element_factory_get_metadata (GST_ELEMENT_FACTORY_CAST (feature),
      GST_ELEMENT_METADATA_KLASS);
  /* only select color space converter */
  if (strstr (klass, "Filter") &&
      strstr (klass, "Converter") && strstr (klass, "Video")) {
    GST_DEBUG_OBJECT (autovideoconvert2,
        "gst_auto_video_convert2_element_filter found %s\n",
        gst_plugin_feature_get_name (GST_PLUGIN_FEATURE_CAST (feature)));
    return TRUE;
  }

  return FALSE;
}

static GList *
create_factory_list (GstAutoVideoConvert2 * autovideoconvert2)
{
  GList *result = NULL;

  /* get the feature list using the filter */
  result = gst_registry_feature_filter (gst_registry_get (),
      (GstPluginFeatureFilter) element_filter, FALSE, autovideoconvert2);

  /* sort on rank and name */
  result = g_list_sort (result, gst_plugin_feature_rank_compare_func);

  return result;
}

static void
update_factory_list (GstAutoVideoConvert2 * autovideoconvert2)
{
  /* use a static mutex to protect factories list and factories cookie */
  g_mutex_lock (&factories_mutex);

  /* test if a factories list already exist or not */
  if (!factories) {
    /* no factories list create it */
    factories_cookie =
        gst_registry_get_feature_list_cookie (gst_registry_get ());
    factories = create_factory_list (autovideoconvert2);
  } else {
    /* a factories list exist but is it up to date? */
    if (factories_cookie !=
        gst_registry_get_feature_list_cookie (gst_registry_get ())) {
      /* we need to update the factories list */
      /* first free the old one */
      gst_plugin_feature_list_free (factories);
      /* then create an updated one */
      factories_cookie =
          gst_registry_get_feature_list_cookie (gst_registry_get ());
      factories = create_factory_list (autovideoconvert2);
    }
  }

  g_mutex_unlock (&factories_mutex);
}

static gboolean
get_caps_image_size (GstCaps * caps, struct Size *size)
{
  guint i;

  for (i = 0; i != gst_caps_get_size (caps); i++) {
    const GstStructure *const s = gst_caps_get_structure (caps, i);
    const gchar *const name = gst_structure_get_name (s);
    if ((strcmp (name, "video/x-raw") == 0 ||
            strcmp (name, "video/x-bayer") == 0) &&
        gst_structure_get_int (s, "width", &size->width) &&
        gst_structure_get_int (s, "height", &size->height))
      return TRUE;
  }

  return FALSE;
}

static gboolean
get_caps_frame_rate (GstCaps * caps, gint * numerator, gint * denominator)
{
  guint i;

  for (i = 0; i != gst_caps_get_size (caps); i++) {
    const GstStructure *const s = gst_caps_get_structure (caps, i);
    if (gst_structure_get_fraction (s, "framerate", numerator, denominator) &&
        *numerator > 0 && *denominator > 0)
      return TRUE;
  }

  return FALSE;
}
