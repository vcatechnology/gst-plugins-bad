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

struct FactoryListEntry
{
  GstStaticPadTemplate *sink_pad_template, *src_pad_template;
  GstCaps *sink_caps, *src_caps;
  GstElementFactory *factory;
};

struct _GstAutoConvert2Priv
{
  /* Lock to prevent caps pipeline structure changes during changes to pads. */
  GMutex lock;

  /* List of element factories with their pad templates and caps constructed. */
  GSList *factory_index;

  /* The union of the caps of all the converter sink caps. */
  GstCaps *sink_caps;

  /* The union of the caps of all the converter src caps. */
  GstCaps *src_caps;
};

static void gst_auto_convert2_constructed (GObject * object);
static void gst_auto_convert2_finalize (GObject * object);
static void gst_auto_convert2_dispose (GObject * object);

static GstPad *gst_auto_convert2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_auto_convert2_release_pad (GstElement * element, GstPad * pad);

static gboolean gst_auto_convert2_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_auto_convert2_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_auto_convert2_src_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static gboolean find_pad_templates (GstElementFactory * factory,
    GstStaticPadTemplate ** sink_pad_template,
    GstStaticPadTemplate ** src_pad_template);
static struct FactoryListEntry *create_factory_index_entry (GstElementFactory *
    factory, GstStaticPadTemplate * sink_pad_template,
    GstStaticPadTemplate * src_pad_template);
static void destroy_factory_list_entry (struct FactoryListEntry *entry);
static void index_factories (GstAutoConvert2 * autoconvert2);

static gboolean query_caps (GstAutoConvert2 * autoconvert2, GstQuery * query,
    GstCaps * factory_caps, GList * pads);

static void build_graph (GstAutoConvert2 * autoconvert2);

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

  gobject_class->constructed =
      GST_DEBUG_FUNCPTR (gst_auto_convert2_constructed);
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_auto_convert2_finalize);
  gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_auto_convert2_dispose);
}

static void
gst_auto_convert2_init (GstAutoConvert2 * autoconvert2)
{
  autoconvert2->priv = g_malloc0 (sizeof (GstAutoConvert2Priv));
  g_mutex_init (&autoconvert2->priv->lock);
}

static void
gst_auto_convert2_constructed (GObject * object)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (object);
  index_factories (autoconvert2);
  G_OBJECT_CLASS (parent_class)->constructed (object);
}

static void
gst_auto_convert2_finalize (GObject * object)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (object);

  g_mutex_clear (&autoconvert2->priv->lock);
  g_free (autoconvert2->priv);
}

static void
gst_auto_convert2_dispose (GObject * object)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (object);
  g_slist_free_full (autoconvert2->priv->factory_index,
      (GDestroyNotify) destroy_factory_list_entry);

  gst_caps_unref (autoconvert2->priv->sink_caps);
  gst_caps_unref (autoconvert2->priv->src_caps);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static GstPad *
gst_auto_convert2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (element);
  GstPad *const pad = gst_ghost_pad_new_no_target_from_template (NULL, templ);

  GST_AUTO_CONVERT2_LOCK (autoconvert2);

  if (GST_PAD_TEMPLATE_DIRECTION (templ) == GST_PAD_SINK) {
    gst_pad_set_event_function (pad,
        GST_DEBUG_FUNCPTR (gst_auto_convert2_sink_event));
    gst_pad_set_query_function (pad,
        GST_DEBUG_FUNCPTR (gst_auto_convert2_sink_query));
  } else {
    gst_pad_set_query_function (pad,
        GST_DEBUG_FUNCPTR (gst_auto_convert2_src_query));
  }

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

static gboolean
gst_auto_convert2_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GList *it;

      gst_pad_store_sticky_event (pad, event);
      GST_AUTO_CONVERT2_LOCK (autoconvert2);

      for (it = GST_ELEMENT (autoconvert2)->sinkpads; it; it = it->next)
        if (!gst_pad_has_current_caps ((GstPad *) it->data))
          break;

      /* If every pad has received a sticky caps event, then we can start
       * building the transformation routes. */
      if (!it)
        build_graph (autoconvert2);

      GST_AUTO_CONVERT2_UNLOCK (autoconvert2);

      break;
    }

    default:
      break;
  }

  return gst_pad_event_default (pad, parent, event);
}

static gboolean
gst_auto_convert2_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      return query_caps (autoconvert2, query, autoconvert2->priv->sink_caps,
          GST_ELEMENT (autoconvert2)->srcpads);

    default:
      break;
  }

  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_auto_convert2_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (parent);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:
      return query_caps (autoconvert2, query, autoconvert2->priv->src_caps,
          GST_ELEMENT (autoconvert2)->sinkpads);

    default:
      return gst_pad_query_default (pad, parent, query);
  }
}

static gboolean
find_pad_templates (GstElementFactory * factory,
    GstStaticPadTemplate ** sink_pad_template,
    GstStaticPadTemplate ** src_pad_template)
{
  const GList *pad_templates =
      gst_element_factory_get_static_pad_templates (factory);
  const GList *it;

  *sink_pad_template = NULL, *src_pad_template = NULL;

  /* Find the source and sink pad templates. */
  for (it = pad_templates; it; it = it->next) {
    GstStaticPadTemplate *const pad_template =
        (GstStaticPadTemplate *) it->data;
    GstStaticPadTemplate **const selected_template =
        (pad_template->direction == GST_PAD_SINK) ?
        sink_pad_template : src_pad_template;

    if (*selected_template) {
      /* Found more than one sink template or source template. Abort. */
      return FALSE;
    }

    *selected_template = pad_template;
  }

  /* Return true if both a sink and src pad template were found. */
  return *sink_pad_template && *src_pad_template;
}

static struct FactoryListEntry *
create_factory_index_entry (GstElementFactory * factory,
    GstStaticPadTemplate * sink_pad_template,
    GstStaticPadTemplate * src_pad_template)
{
  struct FactoryListEntry *entry = g_malloc (sizeof (struct FactoryListEntry));
  entry->sink_pad_template = sink_pad_template;
  entry->src_pad_template = src_pad_template;
  entry->sink_caps = gst_static_caps_get (&sink_pad_template->static_caps);
  entry->src_caps = gst_static_caps_get (&src_pad_template->static_caps);
  g_object_ref ((GObject *) factory);
  entry->factory = factory;
  return entry;
}

static void
destroy_factory_list_entry (struct FactoryListEntry *entry)
{
  gst_caps_unref (entry->sink_caps);
  gst_caps_unref (entry->src_caps);
  g_object_unref (entry->factory);
  g_free (entry);
}

static void
index_factories (GstAutoConvert2 * autoconvert2)
{
  const GstAutoConvert2Class *const klass =
      GST_AUTO_CONVERT2_GET_CLASS (autoconvert2);
  GList *it;
  GSList *i;
  GstStaticPadTemplate *sink_pad_template, *src_pad_template;

  if (!klass->get_factories) {
    GST_ELEMENT_ERROR (autoconvert2, CORE, NOT_IMPLEMENTED,
        ("No get_factories method has been implemented"), (NULL));
    return;
  }

  autoconvert2->priv->sink_caps = gst_caps_new_empty ();
  autoconvert2->priv->src_caps = gst_caps_new_empty ();

  /* Create the factory list entries and identify the pads. */
  for (it = klass->get_factories (autoconvert2); it; it = it->next) {
    GstElementFactory *const factory = GST_ELEMENT_FACTORY (it->data);
    if (find_pad_templates (factory, &sink_pad_template, &src_pad_template)) {
      struct FactoryListEntry *const entry =
          create_factory_index_entry (factory, sink_pad_template,
          src_pad_template);
      autoconvert2->priv->factory_index =
          g_slist_prepend (autoconvert2->priv->factory_index, entry);
    }
  }

  /* Accumulate the union of all caps. */
  for (i = autoconvert2->priv->factory_index; i; i = i->next) {
    struct FactoryListEntry *const entry = (struct FactoryListEntry *) i->data;
    gst_caps_ref (entry->sink_caps);
    autoconvert2->priv->sink_caps =
        gst_caps_merge (autoconvert2->priv->sink_caps, entry->sink_caps);

    gst_caps_ref (entry->src_caps);
    autoconvert2->priv->src_caps =
        gst_caps_merge (autoconvert2->priv->src_caps, entry->src_caps);
  }
}

static gboolean
query_caps (GstAutoConvert2 * autoconvert2, GstQuery * query,
    GstCaps * factory_caps, GList * pads)
{
  GList *it;
  GstCaps *filter;
  GstCaps *caps = gst_caps_new_empty ();

  gst_query_parse_caps (query, &filter);

  GST_AUTO_CONVERT2_LOCK (autoconvert2);
  for (it = pads; it; it = it->next)
    caps = gst_caps_merge (caps,
        gst_pad_peer_query_caps ((GstPad *) it->data, filter));
  GST_AUTO_CONVERT2_UNLOCK (autoconvert2);

  gst_caps_ref (factory_caps);

  if (filter) {
    GstCaps *const filtered_factory_caps = gst_caps_intersect_full (filter,
        factory_caps, GST_CAPS_INTERSECT_FIRST);
    caps = gst_caps_merge (caps, filtered_factory_caps);
  } else {
    caps = gst_caps_merge (caps, factory_caps);
  }

  caps = gst_caps_normalize (caps);
  gst_query_set_caps_result (query, caps);
  gst_caps_unref (caps);

  return TRUE;
}

static void
build_graph (GstAutoConvert2 * autoconvert2)
{
}
