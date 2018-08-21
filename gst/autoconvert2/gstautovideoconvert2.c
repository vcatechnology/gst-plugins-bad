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

struct _GstAutoVideoConvert2Priv
{
};

static void gst_auto_video_convert2_finalize (GObject * object);

static GList *gst_auto_video_convert2_get_factories (GstAutoConvert2 *
    autoconvert2);

static gboolean element_filter (GstPluginFeature * feature,
    GstAutoVideoConvert2 * autovideoconvert2);
static GList *create_factory_list (GstAutoVideoConvert2 * autovideoconvert2);
static void update_factory_list (GstAutoVideoConvert2 * autovideoconvert2);

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
