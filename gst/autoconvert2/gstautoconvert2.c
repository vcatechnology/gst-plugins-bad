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

#include <string.h>

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

struct ChainGenerator
{
  GstCaps *sink_caps, *src_caps;
  guint length;
  GSList **iterators;
  gboolean init;
};

struct ProposalParent
{
  struct Proposal *proposal;
  union
  {
    GstPad *pad;
    unsigned int parent_step;
  };
};

struct Proposal
{
  struct ProposalParent parent;

  GstPad *src_pad;

  guint step_count;
  GstAutoConvert2TransformationStep *steps;
  guint cost;
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

static const guint MaxChainLength = 4;

static void gst_auto_convert2_constructed (GObject * object);
static void gst_auto_convert2_finalize (GObject * object);
static void gst_auto_convert2_dispose (GObject * object);

static gboolean gst_auto_convert2_validate_transform_route (GstAutoConvert2 *
    autoconvert2, const GstAutoConvert2TransformRoute * route);
static int gst_auto_convert2_validate_chain (GstAutoConvert2 * autoconvert2,
    GstCaps * sink_caps, GstCaps * src_caps, GSList ** chain,
    guint chain_length);

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

static int validate_chain_caps (GstAutoConvert2 * autoconvert2,
    GstCaps * chain_sink_caps, GstCaps * chain_src_caps, GSList ** chain,
    guint chain_length);
static int validate_non_consecutive_elements (GstAutoConvert2 * autoconvert2,
    GstCaps * sink_caps, GstCaps * src_caps, GSList ** chain,
    guint chain_length);

static void init_chain_generator (struct ChainGenerator *generator,
    GSList * factory_index,
    const GstAutoConvert2TransformRoute * transform_route, guint length);
static void destroy_chain_generator (struct ChainGenerator *generator);
static gboolean advance_chain_generator (struct ChainGenerator *generator,
    GSList * factory_index, guint starting_depth);
static gboolean generate_next_chain (GstAutoConvert2 * autoconvert2,
    struct ChainGenerator *generator);

static struct Proposal
    *create_proposal (const struct ProposalParent *parent, GstPad * src_pad,
    guint step_count);
static struct Proposal
    *create_costed_proposal_from_instantiated_chain (GstAutoConvert2 *
    autoconvert2, const struct ChainGenerator *gen,
    const struct ProposalParent *parent, GstPad * src_pad,
    GstElement ** elements);
static void destroy_proposal (struct Proposal *proposal);

static GstElement *create_test_element (GstAutoConvert2 * autoconvert2,
    GstElementFactory * factory, guint index);
static GstElement *get_test_element (GstAutoConvert2 * autoconvert2,
    GHashTable * test_element_cache, GstElementFactory * factory);
static void destroy_cache_factory_elements (GSList * entries);

static GstPad *get_element_pad (GstElement * element, const gchar * pad_name);
static void release_element_pad (GstPad * pad);

static gboolean check_instantiated_chain (GstCaps * sink_caps,
    GstPad * chain_sink_pad);

static gboolean test_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);

static struct Proposal *try_chain (GstAutoConvert2 * autoconvert2,
    GHashTable * test_element_cache, struct ChainGenerator *gen,
    const struct ProposalParent *parent, GstCaps * sink_caps, GstPad * src_pad,
    GstCaps * src_caps);
static struct Proposal *try_passthrough (const struct ProposalParent *parent,
    GstCaps * sink_caps, GstPad * src_pad);

static GSList *generate_transform_route_proposals (GstAutoConvert2 *
    autoconvert2, GHashTable * const test_element_cache,
    const GstAutoConvert2TransformRoute * route,
    const struct ProposalParent *parent, GSList * proposals);
static GSList *generate_branch_proposals (GstAutoConvert2 * autoconvert2,
    GHashTable * const test_element_cache, struct Proposal *parent,
    GstPad * src_pad, GSList * proposals);
static GSList *generate_proposals (GstAutoConvert2 * autoconvert2);

static void build_graph (GstAutoConvert2 * autoconvert2);

static GQuark in_use_quark = 0;
static GQuark is_request_pad_quark = 0;
static GQuark src_caps_quark = 0;

#define gst_auto_convert2_parent_class parent_class
G_DEFINE_TYPE (GstAutoConvert2, gst_auto_convert2, GST_TYPE_BIN);

static void
gst_auto_convert2_class_init (GstAutoConvert2Class * klass)
{
  GObjectClass *const gobject_class = (GObjectClass *) klass;
  GstElementClass *const gstelement_class = (GstElementClass *) klass;

  GST_DEBUG_CATEGORY_INIT (autoconvert2_debug, "autoconvert2", 0,
      "autoconvert2 element");

  in_use_quark = g_quark_from_static_string ("in_use");
  is_request_pad_quark = g_quark_from_static_string ("is_request_pad");
  src_caps_quark = g_quark_from_static_string ("src_caps");

  gst_element_class_set_static_metadata (gstelement_class,
      "Selects conversion elements based on caps", "Generic/Bin",
      "Creates a graph of transform elements based on the caps",
      "Joel Holdsworth <joel.holdsworth@vcatechnology.com>");

  gst_element_class_add_static_pad_template (gstelement_class, &srctemplate);
  gst_element_class_add_static_pad_template (gstelement_class, &sinktemplate);

  klass->validate_transform_route =
      GST_DEBUG_FUNCPTR (gst_auto_convert2_validate_transform_route);
  klass->validate_chain = GST_DEBUG_FUNCPTR (gst_auto_convert2_validate_chain);

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

static gboolean
gst_auto_convert2_validate_transform_route (GstAutoConvert2 * autoconvert2,
    const GstAutoConvert2TransformRoute * route)
{
  return TRUE;
}

static int
gst_auto_convert2_validate_chain (GstAutoConvert2 * autoconvert2,
    GstCaps * sink_caps, GstCaps * src_caps, GSList ** chain,
    guint chain_length)
{
  typedef int (*Validator) (GstAutoConvert2 *, GstCaps *, GstCaps *, GSList **,
      guint);
  const Validator validators[] = {
    validate_chain_caps,
    validate_non_consecutive_elements
  };

  guint i;

  for (i = 0; i != sizeof (validators) / sizeof (validators[0]); i++) {
    const int depth = validators[i] (autoconvert2, sink_caps, src_caps, chain,
        chain_length);
    if (depth != -1)
      return depth;
  }

  return -1;
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
init_chain_generator (struct ChainGenerator *generator, GSList * factory_index,
    const GstAutoConvert2TransformRoute * transform_route, guint length)
{
  guint i;

  generator->sink_caps = transform_route->sink.caps;
  gst_caps_ref (generator->sink_caps);
  generator->src_caps = transform_route->src.caps;
  gst_caps_ref (generator->src_caps);

  generator->length = length;
  generator->iterators = g_malloc (sizeof (GSList *) * length);
  for (i = 0; i != length; i++)
    generator->iterators[i] = factory_index;
  generator->init = TRUE;
}

static void
destroy_chain_generator (struct ChainGenerator *generator)
{
  gst_caps_unref (generator->sink_caps);
  gst_caps_unref (generator->src_caps);
  g_free (generator->iterators);
}

static int
validate_chain_caps (GstAutoConvert2 * autoconvert2, GstCaps * chain_sink_caps,
    GstCaps * chain_src_caps, GSList ** chain, guint chain_length)
{
  int depth = chain_length;

  /* Check if this chain's caps can connect, heading in the upstream
   * direction. */
  do {
    const GstCaps *const src_caps = (depth == 0) ? chain_sink_caps :
        ((struct FactoryListEntry *) chain[depth - 1]->data)->src_caps;
    const GstCaps *const sink_caps = (depth == chain_length) ? chain_src_caps :
        ((struct FactoryListEntry *) chain[depth]->data)->sink_caps;

    if (!gst_caps_can_intersect (src_caps, sink_caps))
      break;
  } while (--depth >= 0);

  return depth;
}

static int
validate_non_consecutive_elements (GstAutoConvert2 * autoconvert2,
    GstCaps * sink_caps, GstCaps * src_caps, GSList ** chain,
    guint chain_length)
{
  int depth = 0;
  for (depth = chain_length - 2; depth >= 0; depth--)
    if (chain[depth]->data == chain[depth + 1]->data)
      break;
  return depth;
}

static gboolean
advance_chain_generator (struct ChainGenerator *generator,
    GSList * factory_index, guint starting_depth)
{
  int i;
  const guint len = generator->length;

  /* Advance to the next permutation. */
  for (i = starting_depth; i < len; i++) {
    GSList **const it = generator->iterators + i;
    *it = (*it)->next;
    if (*it)
      break;
    else
      *it = factory_index;
  }

  /* If all the permutations have been tried, the generator is done. */
  if (i == len)
    return FALSE;

  /* Reset all the elements above the starting depth. */
  for (i = 0; i != starting_depth; i++)
    generator->iterators[i] = factory_index;

  return TRUE;
}

static gboolean
generate_next_chain (GstAutoConvert2 * autoconvert2, struct ChainGenerator *gen)
{
  const GstAutoConvert2Class *const klass =
      GST_AUTO_CONVERT2_GET_CLASS (autoconvert2);
  int depth = 0;

  if (!autoconvert2->priv->factory_index)
    return FALSE;

  for (;;) {
    if (gen->init)
      gen->init = FALSE;
    else if (!advance_chain_generator (gen, autoconvert2->priv->factory_index,
            depth))
      return FALSE;

    depth = klass->validate_chain (autoconvert2, gen->sink_caps, gen->src_caps,
        gen->iterators, gen->length);
    if (depth < 0)
      return TRUE;

    if (depth > 0)
      depth--;
  }
}

static struct Proposal *
create_proposal (const struct ProposalParent *parent, GstPad * src_pad,
    guint step_count)
{
  struct Proposal *const p = g_malloc0 (sizeof (struct Proposal));
  if (parent)
    p->parent = *parent;
  p->step_count = step_count;
  p->src_pad = src_pad;
  gst_object_ref (GST_OBJECT (p->src_pad));
  p->steps =
      g_malloc0 (sizeof (GstAutoConvert2TransformationStep) * step_count);
  return p;
}

static struct Proposal *
create_costed_proposal_from_instantiated_chain (GstAutoConvert2 * autoconvert2,
    const struct ChainGenerator *gen, const struct ProposalParent *parent,
    GstPad * src_pad, GstElement ** elements)
{
  const GstAutoConvert2Class *const klass =
      GST_AUTO_CONVERT2_GET_CLASS (autoconvert2);
  struct Proposal *const proposal = create_proposal (parent, src_pad,
      gen->length);
  guint i;
  GstPad *pad;

  for (i = 0; i != gen->length; i++) {
    const GstElement *const element = elements[i];
    const struct FactoryListEntry *const entry = gen->iterators[i]->data;
    GstAutoConvert2TransformationStep *const step = proposal->steps + i;

    step->sink_pad_template = entry->sink_pad_template;
    step->src_pad_template = entry->src_pad_template;

    g_warn_if_fail (element->numsrcpads == 1);
    g_warn_if_fail (element->srcpads);

    pad = (GstPad *) element->sinkpads->data;
    g_warn_if_fail (pad);
    step->sink_caps = gst_pad_get_current_caps (pad);

    pad = (GstPad *) element->srcpads->data;
    g_warn_if_fail (pad);
    step->src_caps = gst_pad_get_current_caps (pad);

    if (!step->sink_caps || !step->src_caps) {
      destroy_proposal (proposal);
      return NULL;
    }

    step->factory = entry->factory;
    gst_object_ref (GST_OBJECT (step->factory));
  }

  for (i = 0; i != gen->length; i++) {
    GstAutoConvert2TransformationStep *const step = proposal->steps + i;
    proposal->cost += klass->cost_transformation_step ?
        klass->cost_transformation_step (autoconvert2, step) : 1;
  }

  return proposal;
}

static void
destroy_proposal (struct Proposal *proposal)
{
  guint i;

  if (!proposal)
    return;

  for (i = 0; i != proposal->step_count; i++) {
    GstAutoConvert2TransformationStep *const step = proposal->steps + i;
    if (step->sink_caps)
      gst_caps_unref (step->sink_caps);
    if (step->src_caps)
      gst_caps_unref (step->src_caps);
    if (step->factory)
      gst_object_unref (GST_OBJECT (step->factory));
  }

  gst_object_unref (GST_OBJECT (proposal->src_pad));
  g_free (proposal->steps);
  g_free (proposal);
}

static GstElement *
create_test_element (GstAutoConvert2 * autoconvert2,
    GstElementFactory * factory, guint index)
{
  gchar *const factory_name = gst_object_get_name (GST_OBJECT (factory));
  gchar *const element_name = g_strdup_printf ("test_%s_%u", factory_name,
      index);
  GstElement *const element =
      gst_element_factory_create (factory, element_name);
  gst_element_set_parent (element, GST_OBJECT (autoconvert2));
  g_free (factory_name);
  g_free (element_name);

  return element;
}

static GstElement *
get_test_element (GstAutoConvert2 * autoconvert2,
    GHashTable * test_element_cache, GstElementFactory * factory)
{
  GstElement *element = NULL;
  GSList *it, *factory_elements =
      g_hash_table_lookup (test_element_cache, factory);

  for (it = factory_elements; it; it = it->next)
    if (!g_object_get_qdata (G_OBJECT (it->data), in_use_quark)) {
      element = (GstElement *) it->data;
      break;
    }

  if (!element) {
    g_hash_table_steal (test_element_cache, factory);
    element = create_test_element (autoconvert2, factory,
        g_slist_length (factory_elements));
    factory_elements = g_slist_prepend (factory_elements, element);
    g_hash_table_replace (test_element_cache, factory, factory_elements);
  }

  g_object_set_qdata (G_OBJECT (element), in_use_quark, GINT_TO_POINTER (TRUE));

  return element;
}

static void
destroy_cache_factory_elements (GSList * entries)
{
  GSList *it;
  for (it = entries; it; it = it->next) {
    GstElement *const e = (GstElement *) it->data;
    gst_element_set_state (e, GST_STATE_NULL);
    gst_object_unparent (GST_OBJECT (e));
  }

  g_slist_free (entries);
}

static GstPad *
get_element_pad (GstElement * element, const gchar * pad_name)
{
  GstPad *pad = NULL;

  g_warn_if_fail (pad_name);

  if (!(pad = gst_element_get_static_pad (element, pad_name)))
    if ((pad = gst_element_get_request_pad (element, pad_name)))
      g_object_set_qdata (G_OBJECT (pad), is_request_pad_quark,
          GINT_TO_POINTER (TRUE));

  return pad;
}

static void
release_element_pad (GstPad * pad)
{
  GstElement *const element = gst_pad_get_parent_element (pad);

  g_warn_if_fail (pad);
  if (g_object_get_qdata (G_OBJECT (pad), is_request_pad_quark)) {
    gst_object_ref (pad);
    g_warn_if_fail (element);
    gst_element_release_request_pad (element, pad);
    g_object_set_qdata (G_OBJECT (pad), is_request_pad_quark,
        GINT_TO_POINTER (FALSE));
    gst_object_unref (pad);
  }
  gst_object_unref (element);
}

static gboolean
check_instantiated_chain (GstCaps * sink_caps, GstPad * chain_sink_pad)
{
  GstCaps *const chain_sink_caps = gst_pad_query_caps (chain_sink_pad, NULL);
  const gboolean can_intersect =
      gst_caps_can_intersect (chain_sink_caps, sink_caps);
  gst_caps_unref (chain_sink_caps);
  return can_intersect;
}

static gboolean
test_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_CAPS:{
      GstCaps *const caps = g_object_get_qdata (G_OBJECT (pad), src_caps_quark);
      gst_query_set_caps_result (query, caps);
      return TRUE;
    }

    default:
      return gst_pad_query_default (pad, parent, query);
  }
}

static struct Proposal *
try_chain (GstAutoConvert2 * autoconvert2, GHashTable * test_element_cache,
    struct ChainGenerator *gen, const struct ProposalParent *parent,
    GstCaps * sink_caps, GstPad * src_pad, GstCaps * src_caps)
{
  guint i;
  GstElement **elements = g_malloc0 (sizeof (GstElement *) * gen->length);
  GstPad **element_sink_pads = g_malloc0 (sizeof (GstPad *) * gen->length);
  GstPad **element_src_pads = g_malloc0 (sizeof (GstPad *) * gen->length);
  GstPad *test_sink_pad = NULL;
  struct Proposal *proposal = NULL;

  /* Create the and link the elements. */
  for (i = 0; i != gen->length; i++) {
    struct FactoryListEntry *const entry = gen->iterators[i]->data;
    elements[i] = get_test_element (autoconvert2, test_element_cache,
        entry->factory);
    gst_element_sync_state_with_parent (elements[i]);

    element_sink_pads[i] =
        get_element_pad (elements[i], entry->sink_pad_template->name_template);
    element_src_pads[i] =
        get_element_pad (elements[i], entry->src_pad_template->name_template);

    if (i != 0) {
      const GstPadLinkReturn ret = gst_pad_link_full (element_src_pads[i - 1],
          element_sink_pads[i],
          GST_PAD_LINK_CHECK_NOTHING | GST_PAD_LINK_CHECK_NO_RECONFIGURE);
      if (ret != GST_PAD_LINK_OK)
        goto abort;
    }
  }

  /* Link it to a dummy pad that will represent the down-stream element. */
  test_sink_pad = gst_pad_new ("test_sink_pad", GST_PAD_SINK);
  gst_pad_set_active (test_sink_pad, TRUE);
  g_object_set_qdata (G_OBJECT (test_sink_pad), src_caps_quark, src_caps);
  gst_pad_set_query_function (test_sink_pad, test_sink_query);

  gst_pad_link_full (element_src_pads[gen->length - 1], test_sink_pad,
      GST_PAD_LINK_CHECK_NOTHING | GST_PAD_LINK_CHECK_NO_RECONFIGURE);

  /* Test if the caps are compatible with the chain. */
  if (check_instantiated_chain (sink_caps, element_sink_pads[0])) {
    /* Send a caps event so that the elements apply the caps. */
    if (gst_pad_send_event (element_sink_pads[0],
            gst_event_new_caps (sink_caps))) {
      /* If the caps applied successfully, create a costed proposal from the
       * the element chain. */
      proposal = create_costed_proposal_from_instantiated_chain (autoconvert2,
          gen, parent, src_pad, elements);
    }
  }

  /* Tidy up. */
abort:
  if (test_sink_pad) {
    gst_pad_set_active (test_sink_pad, FALSE);
    gst_pad_unlink (element_src_pads[gen->length - 1], test_sink_pad);
    gst_object_unref (test_sink_pad);
  }

  if (elements[0]) {
    for (i = 1; i != gen->length; i++) {
      if (!elements[i])
        break;
      gst_pad_unlink (element_src_pads[i - 1], element_sink_pads[i]);
    }
  }

  for (i = 0; i != gen->length; i++) {
    release_element_pad (element_sink_pads[i]);
    gst_object_unref (element_sink_pads[i]);
    release_element_pad (element_src_pads[i]);
    gst_object_unref (element_src_pads[i]);

    if (elements[i])
      g_object_set_qdata (G_OBJECT (elements[i]), in_use_quark,
          GINT_TO_POINTER (FALSE));
  }

  g_free (elements);
  g_free (element_sink_pads);
  g_free (element_src_pads);

  return proposal;
}

static struct Proposal *
try_passthrough (const struct ProposalParent *parent, GstCaps * sink_caps,
    GstPad * src_pad)
{
  struct Proposal *proposal = NULL;
  GstPad *const src_peer_pad = gst_pad_get_peer (src_pad);
  if (check_instantiated_chain (sink_caps, src_peer_pad))
    proposal = create_proposal (parent, src_pad, 0);
  gst_object_unref (GST_OBJECT (src_peer_pad));
  return proposal;
}

static GSList *
generate_transform_route_proposals (GstAutoConvert2 * autoconvert2,
    GHashTable * const test_element_cache,
    const GstAutoConvert2TransformRoute * route,
    const struct ProposalParent *parent, GSList * proposals)
{
  GSList *orig_proposals = proposals;
  struct Proposal *proposal;
  guint length;
  struct ChainGenerator generator;

  if (!GST_AUTO_CONVERT2_GET_CLASS (autoconvert2)->validate_transform_route
      (autoconvert2, route))
    return proposals;

  if ((proposal = try_passthrough (parent, route->sink.caps, route->src.pad))) {
    proposals = g_slist_prepend (proposals, proposal);
  } else {
    for (length = 1; length <= MaxChainLength && proposals == orig_proposals;
        length++) {
      init_chain_generator (&generator, autoconvert2->priv->factory_index,
          route, length);
      while (generate_next_chain (autoconvert2, &generator)) {
        if ((proposal = try_chain (autoconvert2, test_element_cache,
                    &generator, parent, route->sink.caps, route->src.pad,
                    route->src.caps))) {
          proposals = g_slist_prepend (proposals, proposal);
        }
      }
      destroy_chain_generator (&generator);
    }
  }

  return proposals;
}

static GSList *
generate_branch_proposals (GstAutoConvert2 * autoconvert2,
    GHashTable * const test_element_cache, struct Proposal *parent,
    GstPad * src_pad, GSList * proposals)
{
  const struct Proposal *proposal;
  unsigned int i;
  GstCaps *const src_caps = gst_pad_peer_query_caps (src_pad, NULL);

  g_warn_if_fail (parent);

  /* Check the pad is not already attached to a parent proposal. */
  for (proposal = parent; proposal; proposal = proposal->parent.proposal)
    if (proposal->src_pad == src_pad)
      goto done;

  /* Generate the best proposal if possible. */
  for (i = 0; i != parent->step_count; i++) {
    GstCaps *const sink_caps = parent->steps[i].src_caps;
    const GstAutoConvert2TransformRoute transform_route = {
      {NULL, sink_caps}, {src_pad, src_caps}
    };
    const struct ProposalParent p = {
      .proposal = parent,
      .parent_step = i
    };

    proposals = generate_transform_route_proposals (autoconvert2,
        test_element_cache, &transform_route, &p, proposals);
  };

done:
  gst_caps_unref (src_caps);

  return proposals;
}

static GSList *
generate_proposals (GstAutoConvert2 * autoconvert2)
{
  GList *i;
  GSList *proposals = NULL, *proposal_yield = NULL, *prev_proposal_yield = NULL;
  GHashTable *const test_element_cache =
      g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) destroy_cache_factory_elements);

  /* Generate direct sink-pad to source-pad proposals. */
  for (i = GST_ELEMENT (autoconvert2)->srcpads; i; i = i->next) {
    GstPad *const src_pad = (GstPad *) i->data;
    GstCaps *const src_caps = gst_pad_peer_query_caps (src_pad, NULL);
    GList *j;

    for (j = GST_ELEMENT (autoconvert2)->sinkpads; j; j = j->next) {
      GstPad *const sink_pad = (GstPad *) j->data;
      GstCaps *const sink_caps = gst_pad_get_current_caps (sink_pad);
      const GstAutoConvert2TransformRoute transform_route = {
        {sink_pad, sink_caps}, {src_pad, src_caps}
      };
      const struct ProposalParent p = {
        .proposal = NULL,
        .pad = sink_pad
      };

      proposal_yield = generate_transform_route_proposals (autoconvert2,
          test_element_cache, &transform_route, &p, proposal_yield);

      gst_caps_unref (sink_caps);
    }

    gst_caps_unref (src_caps);
  }

  while (proposal_yield) {
    proposals = g_slist_concat (proposals, g_slist_copy (proposal_yield));
    g_slist_free (prev_proposal_yield);
    prev_proposal_yield = proposal_yield;
    proposal_yield = NULL;

    /* For every proposal in the previous generation, generate branches,
     * that link to every src pad. */
    for (i = GST_ELEMENT (autoconvert2)->srcpads; i; i = i->next) {
      GstPad *const src_pad = (GstPad *) i->data;
      GSList *j;

      for (j = prev_proposal_yield; j; j = j->next)
        proposal_yield = generate_branch_proposals (autoconvert2,
            test_element_cache, (struct Proposal *) j->data, src_pad,
            proposal_yield);
    }
  }

  g_hash_table_destroy (test_element_cache);

  return proposals;
}

static void
build_graph (GstAutoConvert2 * autoconvert2)
{
  GSList *const proposals = generate_proposals (autoconvert2);
  g_slist_free_full (proposals, (GDestroyNotify) destroy_proposal);
}
