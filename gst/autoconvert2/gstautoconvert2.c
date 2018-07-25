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
 * The #autoconvert2 element has sink and source request pads. The element will
 * attempt to construct a graph of conversion elements that will convert from
 * the input caps to the output caps in the most efficient manner possible. The
 * incoming streams fed into the sink pads are assumed to represent related
 * input data but represented in different forms e.g. a video stream where the
 * frames are available in different frame sizes.
 *
 * If the caps change, the element will replace the network with another that
 * will convert to the new caps.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstautoconvert2.h"

GST_DEBUG_CATEGORY (autoconvert2_debug);
#define GST_CAT_DEFAULT (autoconvert2_debug)

#define GST_AUTO_CONVERT2_GET_LOCK(autoconvert2) \
  (&GST_AUTO_CONVERT2(autoconvert2)->priv->lock)
#define GST_AUTO_CONVERT2_LOCK(autoconvert2) \
  (g_mutex_lock (GST_AUTO_CONVERT2_GET_LOCK (autoconvert2)))
#define GST_AUTO_CONVERT2_UNLOCK(autoconvert2) \
  (g_mutex_unlock (GST_AUTO_CONVERT2_GET_LOCK (autoconvert2)))

static GstStaticPadTemplate sinktemplate = GST_STATIC_PAD_TEMPLATE ("sink_%u",
    GST_PAD_SINK,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src_%u",
    GST_PAD_SRC,
    GST_PAD_REQUEST,
    GST_STATIC_CAPS_ANY);

struct _GstAutoConvert2Priv
{
  /* Lock to prevent caps pipeline structure changes during changes to pads. */
  GMutex lock;
};

static void gst_auto_convert2_finalize (GObject * object);

static GstPad *gst_auto_convert2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_auto_convert2_release_pad (GstElement * element, GstPad * pad);

#define gst_auto_convert2_parent_class parent_class
G_DEFINE_TYPE (GstAutoConvert2, gst_auto_convert2, GST_TYPE_BIN);

static void
gst_auto_convert2_class_init (GstAutoConvert2Class * klass)
{
  GObjectClass *const gobject_class = (GObjectClass *) klass;
  GstElementClass *const gstelement_class = (GstElementClass *) klass;

  GST_DEBUG_CATEGORY_INIT (autoconvert2_debug, "autoconvert2", 0,
      "autoconvert2 element");

  gst_element_class_set_static_metadata (gstelement_class,
      "Selects conversion elements based on caps", "Generic/Bin",
      "Creates a graph of transform elements based on the caps",
      "Joel Holdsworth <joel.holdsworth@vcatechnology.com>");

  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  gstelement_class->request_new_pad =
      GST_DEBUG_FUNCPTR (gst_auto_convert2_request_new_pad);
  gstelement_class->release_pad =
      GST_DEBUG_FUNCPTR (gst_auto_convert2_release_pad);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_auto_convert2_finalize);
}

static void
gst_auto_convert2_init (GstAutoConvert2 * autoconvert2)
{
  autoconvert2->priv = g_malloc0 (sizeof (GstAutoConvert2Priv));
  g_mutex_init (&autoconvert2->priv->lock);
}

static void
gst_auto_convert2_finalize (GObject * object)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (object);

  g_mutex_clear (&autoconvert2->priv->lock);
  g_free (autoconvert2->priv);
}

static GstPad *
gst_auto_convert2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (element);
  GstPad *const pad = gst_ghost_pad_new_no_target_from_template (NULL, templ);

  GST_AUTO_CONVERT2_LOCK (autoconvert2);

  if (gst_element_add_pad (element, pad)) {
    GST_AUTO_CONVERT2_UNLOCK (autoconvert2);
    return pad;
  }

  GST_DEBUG_OBJECT (autoconvert2, "could not add pad");
  gst_object_unref (pad);
  GST_AUTO_CONVERT2_UNLOCK (autoconvert2);
  return NULL;
}

static void
gst_auto_convert2_release_pad (GstElement * element, GstPad * pad)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (element);

  GST_AUTO_CONVERT2_LOCK (autoconvert2);
  gst_element_remove_pad (element, pad);
  GST_AUTO_CONVERT2_UNLOCK (autoconvert2);
}
