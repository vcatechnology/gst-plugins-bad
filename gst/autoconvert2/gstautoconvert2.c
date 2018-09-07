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
 *
 * # Method
 *
 * When the element is first initialized, it retrieves a list of element
 * factories from the derrived class for by calling the #get_factories method.
 * These factories are then scanned to ensure they have one input pad template,
 * and one output pad template, and the factories and their caps are stored
 * in a list: #factory_index .
 *
 * When querying the caps of the element's source and sink pads it will
 * advertise the ability to accept the union of the caps on opposite-facing
 * pads and the pads of all the listed factories.
 *
 * When the caps of all the sink pads have been set by the caps event, the
 * element will calculate the graph of child elements necessary to efficiently
 * provide the output data required by all the source pads.
 *
 * For each source pad, the element considers a "transformation route" beginning
 * from each of the sink pads. The derrived class can validate whether each
 * potential route will be allowed with the #validate_transformation_route
 * method. For example, the sink pads provide 1080p and 480p imagery and 1080p
 * imagery is required in the source pads, it would be undesirable to allow
 * transformation routes which source data from the 480p sink pad.
 *
 * Then, for each transformation route, the element begins by checking whether
 * a passthrough is possible. If not it attempts to construct chains of elements
 * of increasing length, by assembling every possible permutation of elements.
 * However, many permutations are rejected. For example, elements with
 * incompatible caps, and chains where two or more copies of the same type of
 * element are connected in series. The derrived class also has an opportunity
 * to black-list chains of elements with the #validate_chain method.
 *
 * Each chain that passes validation is then instantiated as a sequence of
 * test elements which are linked together, and then commanded to negotiate
 * their caps through a caps event sent into the sink pad the chain.
 *
 * After the caps have been negotiated by each element in the test chain,
 * the factory and fixated caps of each element are provided to the derrived
 * so that it can compute a computation cost of each element. This is an
 * arbitrary value that is used to determine the relative efficiency of one
 * method of conversion versus another. The test chain is then deconstructed,
 * and the element factory sequence, and the total cost are stored in a list as
 * a #Proposal .
 *
 * Once costed proposals have been generated for every allowed direct
 * transformation route, branched transformation routes are considered. For
 * each costed proposal, transformation routes are generated that are attached
 * to every pad along the parent proposal's chain. The branch proposals are
 * similarly validated, tested and costed.
 *
 * The optimal set of proposals is then selected by means of a dynamic
 * programming algorithm, that searches for the lowest cost set of direct and
 * branch proposals that can satisfy the source pad caps.
 *
 * Once this set has been calculated, the final set of elements is instantiated
 * and linked.
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

enum Klasses
{
  CONVERTER = 1 << 0,
  DECODER = 1 << 1,
  ENCODER = 1 << 2,
  PARSER = 1 << 3
};

static const gchar *KlassStrings[] = {
  "Converter",
  "Decoder",
  "Encoder",
  "Parser"
};

struct FactoryListEntry
{
  GstStaticPadTemplate *sink_pad_template, *src_pad_template;
  GstCaps *sink_caps, *src_caps;
  GstElementFactory *factory;
  guint klass_mask;
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

  GSList **step_children;
  GstElement **tee_elements;
  GstPad *chain_sink_pad, *chain_src_pad;

  guint cost;
};

enum BuildState
{
  IDLE,
  DRAINING_GRAPH,
  REBUILDING_GRAPH
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

  volatile enum BuildState state;

  GCond sink_block_cond;

  GHashTable *pending_drain_pads;
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
static void gst_auto_convert2_begin_building_graph (GstAutoConvert2 *
    autoconvert2);

static GstPad *gst_auto_convert2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps);
static void gst_auto_convert2_release_pad (GstElement * element, GstPad * pad);

static GstFlowReturn gst_auto_convert2_chain (GstPad * pad, GstObject * parent,
    GstBuffer * buffer);
static gboolean gst_auto_convert2_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event);
static gboolean gst_auto_convert2_sink_query (GstPad * pad, GstObject * parent,
    GstQuery * query);
static gboolean gst_auto_convert2_src_proxy_event (GstPad * pad,
    GstObject * parent, GstEvent * event);
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

static void enter_build_state (GstAutoConvert2 * autoconvert2,
    enum BuildState prev_state, enum BuildState state);

static void check_sink_block (GstAutoConvert2 * autoconvert2);
static gboolean query_caps (GstAutoConvert2 * autoconvert2, GstQuery * query,
    GstCaps * factory_caps, GList * pads);

static int validate_chain_caps (GstAutoConvert2 * autoconvert2,
    GstCaps * chain_sink_caps, GstCaps * chain_src_caps, GSList ** chain,
    guint chain_length);
static int validate_non_consecutive_elements (GstAutoConvert2 * autoconvert2,
    GstCaps * sink_caps, GstCaps * src_caps, GSList ** chain,
    guint chain_length);
static int validate_element_order (GstAutoConvert2 * autoconvert2,
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

static gboolean set_ghost_pad_target_no_reconfigure (GstGhostPad * pad,
    GstPad * newtarget);
static void release_ghost_pad (GstGhostPad * pad);

static gboolean forward_sticky_events (GstPad * pad, GstEvent ** event,
    gpointer user_data);

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

static GHashTable *index_pads (GList * pad_list, guint * pad_count);
static GSList *select_proposals (GstAutoConvert2 * autoconvert2,
    GSList * proposals);

static void instantiate_proposals (GstAutoConvert2 * autoconvert2,
    GSList * proposals);

static void build_graph (GstAutoConvert2 * autoconvert2);
static void clear_graph (GstAutoConvert2 * autoconvert2);
static void begin_rebuilding_graph (GstAutoConvert2 * autoconvert2);

static void graph_drained (GstAutoConvert2 * autoconvert2);

static gboolean needs_reconfigure (GstAutoConvert2 * autoconvert2);

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
  klass->begin_building_graph =
      GST_DEBUG_FUNCPTR (gst_auto_convert2_begin_building_graph);

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
  autoconvert2->priv->state = IDLE;
  g_cond_init (&autoconvert2->priv->sink_block_cond);
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
  g_cond_clear (&autoconvert2->priv->sink_block_cond);
  g_free (autoconvert2->priv);
}

static void
gst_auto_convert2_dispose (GObject * object)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (object);
  GList *it;

  clear_graph (autoconvert2);

  for (it = GST_ELEMENT (autoconvert2)->pads; it; it = it->next) {
    GstGhostPad *const pad = (GstGhostPad *) it->data;
    release_ghost_pad (pad);
    gst_element_remove_pad (GST_ELEMENT (autoconvert2), GST_PAD (pad));
  }

  g_slist_free_full (autoconvert2->priv->factory_index,
      (GDestroyNotify) destroy_factory_list_entry);

  gst_caps_unref (autoconvert2->priv->sink_caps);
  gst_caps_unref (autoconvert2->priv->src_caps);

  if (autoconvert2->priv->pending_drain_pads)
    g_hash_table_destroy (autoconvert2->priv->pending_drain_pads);

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
    validate_non_consecutive_elements,
    validate_element_order
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

static void
gst_auto_convert2_begin_building_graph (GstAutoConvert2 * autoconvert2)
{
}

static GstPad *
gst_auto_convert2_request_new_pad (GstElement * element,
    GstPadTemplate * templ, const gchar * name, const GstCaps * caps)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (element);
  GstPad *const pad = gst_ghost_pad_new_no_target_from_template (NULL, templ);

  GST_AUTO_CONVERT2_LOCK (autoconvert2);

  if (GST_PAD_TEMPLATE_DIRECTION (templ) == GST_PAD_SINK) {
    gst_pad_set_chain_function (pad,
        GST_DEBUG_FUNCPTR (gst_auto_convert2_chain));
    gst_pad_set_event_function (pad,
        GST_DEBUG_FUNCPTR (gst_auto_convert2_sink_event));
    gst_pad_set_query_function (pad,
        GST_DEBUG_FUNCPTR (gst_auto_convert2_sink_query));
  } else {
    GstProxyPad *const proxy_pad =
        gst_proxy_pad_get_internal (GST_PROXY_PAD (pad));
    gst_pad_set_event_function (GST_PAD (proxy_pad),
        GST_DEBUG_FUNCPTR (gst_auto_convert2_src_proxy_event));
    gst_object_unref (proxy_pad);

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

static GstFlowReturn
gst_auto_convert2_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (parent);

  check_sink_block (autoconvert2);

  if (needs_reconfigure (autoconvert2))
    begin_rebuilding_graph (autoconvert2);

  return gst_proxy_pad_chain_default (pad, parent, buffer);
}

static gboolean
gst_auto_convert2_sink_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstAutoConvert2 *const autoconvert2 = GST_AUTO_CONVERT2 (parent);

  check_sink_block (autoconvert2);

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
      if (!it) {
        GST_AUTO_CONVERT2_GET_CLASS (autoconvert2)->begin_building_graph
            (autoconvert2);
        build_graph (autoconvert2);
      }

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

  check_sink_block (autoconvert2);

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
gst_auto_convert2_src_proxy_event (GstPad * pad, GstObject * parent,
    GstEvent * event)
{
  GstPad *const src_pad = GST_PAD (parent);
  GstAutoConvert2 *const autoconvert2 =
      (GstAutoConvert2 *) gst_pad_get_parent_element (src_pad);
  gboolean drop = FALSE;

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS &&
      g_atomic_int_get (&autoconvert2->priv->state) == DRAINING_GRAPH) {
    gboolean last_pad_drained = TRUE;

    GST_AUTO_CONVERT2_LOCK (autoconvert2);

    g_warn_if_fail (autoconvert2->priv->pending_drain_pads);
    drop = g_hash_table_remove (autoconvert2->priv->pending_drain_pads,
        src_pad);
    last_pad_drained =
        g_hash_table_size (autoconvert2->priv->pending_drain_pads) == 0;

    GST_AUTO_CONVERT2_UNLOCK (autoconvert2);

    if (last_pad_drained)
      graph_drained (autoconvert2);
  }

  gst_object_unref (autoconvert2);

  return drop ? TRUE : gst_pad_event_default (pad, parent, event);
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

  /* Index the klasses. */
  for (i = autoconvert2->priv->factory_index; i; i = i->next) {
    guint j;
    struct FactoryListEntry *const entry = (struct FactoryListEntry *) i->data;
    const gchar *const klass = gst_element_factory_get_metadata (entry->factory,
        GST_ELEMENT_METADATA_KLASS);

    entry->klass_mask = 0;
    for (j = 0; j != sizeof (KlassStrings) / sizeof (KlassStrings[0]); j++)
      if (strstr (klass, KlassStrings[j]))
        entry->klass_mask |= 1 << j;
  }
}

static void
enter_build_state (GstAutoConvert2 * autoconvert2, enum BuildState prev_state,
    enum BuildState state)
{
  g_warn_if_fail (g_atomic_int_compare_and_exchange (&autoconvert2->priv->state,
          prev_state, state));
}

static void
check_sink_block (GstAutoConvert2 * autoconvert2)
{
  GST_AUTO_CONVERT2_LOCK (autoconvert2);
  while (g_atomic_int_get (&autoconvert2->priv->state) != IDLE)
    g_cond_wait (&autoconvert2->priv->sink_block_cond,
        &autoconvert2->priv->lock);
  GST_AUTO_CONVERT2_UNLOCK (autoconvert2);
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
  int depth;
  for (depth = chain_length - 2; depth >= 0; depth--)
    if (chain[depth]->data == chain[depth + 1]->data)
      break;
  return depth;
}

static int
validate_element_order (GstAutoConvert2 * autoconvert2,
    GstCaps * sink_caps, GstCaps * src_caps, GSList ** chain,
    guint chain_length)
{
  const guint stage_klasses[] = { PARSER, DECODER, CONVERTER, ENCODER };
  const guint stage_count = sizeof (stage_klasses) / sizeof (stage_klasses[0]);

  int prev_stage = stage_count - 1;
  int depth, stage;

  for (depth = chain_length - 1; depth >= 0; depth--) {
    const struct FactoryListEntry *const entry =
        (const struct FactoryListEntry *) chain[depth]->data;

    for (stage = 0; stage != stage_count; stage++)
      if (entry->klass_mask & stage_klasses[stage])
        break;

    if (stage > prev_stage)
      break;

    prev_stage = stage;
  }

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
set_ghost_pad_target_no_reconfigure (GstGhostPad * pad, GstPad * newtarget)
{
  GstPadLinkReturn lret;
  GstPad *const internal =
      GST_PAD (gst_proxy_pad_get_internal (GST_PROXY_PAD (pad)));

  g_warn_if_fail (gst_ghost_pad_get_target (GST_GHOST_PAD (pad)) == NULL);
  if (GST_PAD_IS_SRC (internal))
    lret = gst_pad_link_full (internal, newtarget, GST_PAD_LINK_CHECK_NOTHING |
        GST_PAD_LINK_CHECK_NO_RECONFIGURE);
  else
    lret = gst_pad_link_full (newtarget, internal, GST_PAD_LINK_CHECK_NOTHING |
        GST_PAD_LINK_CHECK_NO_RECONFIGURE);

  gst_object_unref (internal);

  if (lret != GST_PAD_LINK_OK) {
    GST_WARNING_OBJECT (pad, "could not link internal and target, reason:%s",
        gst_pad_link_get_name (lret));
    return FALSE;
  }

  return TRUE;
}

static void
release_ghost_pad (GstGhostPad * pad)
{
  GstPad *const target_pad = gst_ghost_pad_get_target (GST_GHOST_PAD (pad));
  if (target_pad) {
    gst_ghost_pad_set_target (GST_GHOST_PAD (pad), NULL);
    release_element_pad (target_pad);
    gst_object_unref (target_pad);
  }
}

static gboolean
forward_sticky_events (GstPad * pad, GstEvent ** event, gpointer user_data)
{
  if (GST_EVENT_TYPE (*event) != GST_EVENT_EOS) {
    GstPad *const proxy =
        GST_PAD (gst_proxy_pad_get_internal (GST_PROXY_PAD (pad)));
    gst_pad_push_event (proxy, gst_event_ref (*event));
    gst_object_unref (GST_PAD (proxy));
  }

  return TRUE;
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

static GHashTable *
index_pads (GList * pad_list, guint * pad_count)
{
  const GList *it;
  guint index = 0;
  GHashTable *const table = g_hash_table_new (NULL, NULL);
  for (it = pad_list; it; it = it->next)
    g_hash_table_insert (table, it->data, GUINT_TO_POINTER (index++));
  *pad_count = index;
  return table;
}

static GSList *
select_proposals (GstAutoConvert2 * autoconvert2, GSList * proposals)
{
  guint set, subset;
  const GSList *it;
  guint src_count;
  GHashTable *const src_id_table =
      index_pads (GST_ELEMENT (autoconvert2)->srcpads, &src_count);
  GSList *result, **selected_proposals =
      g_malloc0 (sizeof (GHashTable *) * (1 << src_count));
  guint *min_costs = g_malloc (sizeof (guint) * (1 << src_count));

  /* Fill the cost matrix with UINT_MAX i.e. all unspecified links have
   * infinite cost. */
  memset (min_costs, 0xFF, sizeof (guint) * (1 << src_count));

  /* First populate the cost table with the proposals. */
  for (it = proposals; it; it = it->next) {
    struct Proposal *p;
    GSList *selection = NULL;
    guint src_set = 0, cost = 0;
    for (p = (struct Proposal *) it->data; p; p = p->parent.proposal) {
      selection = g_slist_prepend (selection, p);
      src_set |=
          1 << GPOINTER_TO_UINT (g_hash_table_lookup (src_id_table,
              p->src_pad));
      cost += p->cost;
    }

    if (cost < min_costs[src_set]) {
      min_costs[src_set] = cost;
      selected_proposals[src_set] = selection;
    }
  }

  /* For every possible set of source pads, divide that set in half in every
   * possible way. Then, if the cost of the subset plus the cost of the
   * remainder is lower than the current cost of the currently selected set of
   * proposals, use these proposals instead. Continue to expand the set of pads
   * until the optimal set of proposals is determined. */
  for (set = 1; set != (1 << src_count); set++) {
    guint cost = min_costs[set];
    GSList *selected = selected_proposals[set];
    for (subset = set; subset != 0; subset = (subset - 1) & set) {
      const guint32 other_subset = set ^ subset;
      const guint subset_cost = min_costs[subset];
      const guint other_subset_cost = min_costs[other_subset];
      if (subset_cost != UINT_MAX && other_subset_cost != UINT_MAX) {
        const guint alt_cost = subset_cost + other_subset_cost;
        if (alt_cost < cost) {
          g_slist_free (selected);
          selected = g_slist_concat (g_slist_copy (selected_proposals[subset]),
              g_slist_copy (selected_proposals[other_subset]));
          cost = alt_cost;
        }
      }
    }
    selected_proposals[set] = selected;
    min_costs[set] = cost;
  }

  result = selected_proposals[(1 << src_count) - 1];

  /* Tidy up. */
  for (set = 0; set != (1 << src_count) - 1; set++)
    g_slist_free (selected_proposals[set]);
  g_free (min_costs);
  g_free (selected_proposals);
  g_hash_table_destroy (src_id_table);

  return result;
}

static void
instantiate_proposals (GstAutoConvert2 * autoconvert2, GSList * proposals)
{
  GHashTable *const sink_pad_tees = g_hash_table_new (NULL, NULL);
  GList *i;
  GSList *it;

  /* Index the children of each sink pad and the children of each proposal. */
  for (it = proposals; it; it = it->next) {
    struct Proposal *const p = (struct Proposal *) it->data;
    struct Proposal *const parent = p->parent.proposal;

    if (parent) {
      GSList **step_children = parent->step_children;
      const guint step = p->parent.parent_step;

      if (!step_children) {
        step_children = g_malloc0 (sizeof (GSList *) * parent->step_count);
        parent->step_children = step_children;
        parent->tee_elements = g_malloc0 (sizeof (GstElement *) *
            parent->step_count);
      }

      step_children[step] = g_slist_prepend (step_children[step], p);
    } else {
      GstPad *const pad = p->parent.pad;
      GstElement *tee = NULL;

      /* Create a table of in-use sink pads and associated tee elements. A tee
       * will created if more than one element connects to the sink pad, or if
       * the element connects directly through to one or more source pads. */
      if ((g_hash_table_lookup_extended (sink_pad_tees, pad, NULL,
                  (gpointer *) & tee) || p->step_count == 0) && !tee) {
        GstPad *target_pad;

        tee = gst_element_factory_make ("tee", NULL);
        gst_bin_add (GST_BIN (autoconvert2), tee);
        gst_element_sync_state_with_parent (tee);
        target_pad = get_element_pad (tee, "sink");
        set_ghost_pad_target_no_reconfigure (GST_GHOST_PAD (pad), target_pad);
        gst_object_unref (target_pad);
      }

      g_hash_table_replace (sink_pad_tees, pad, tee);
    }
  }

  /* Create the chains with tee elements for the attachment points. */
  for (it = proposals; it; it = it->next) {
    struct Proposal *const p = (struct Proposal *) it->data;
    GstPad *src_pad = NULL;
    guint j;

    for (j = 0; j != p->step_count; j++) {
      const GstAutoConvert2TransformationStep *const s = p->steps + j;
      GstElement *const element = gst_element_factory_create (s->factory, NULL);
      GstPad *const sink_pad = get_element_pad (element,
          s->sink_pad_template->name_template);

      gst_bin_add (GST_BIN (autoconvert2), element);
      gst_element_sync_state_with_parent (element);

      if (src_pad) {
        gst_pad_link_full (src_pad, sink_pad, GST_PAD_LINK_CHECK_NOTHING |
            GST_PAD_LINK_CHECK_NO_RECONFIGURE);
        gst_object_unref (src_pad);
        gst_object_unref (sink_pad);
      } else
        p->chain_sink_pad = sink_pad;

      src_pad = get_element_pad (element, s->src_pad_template->name_template);

      if (p->step_children && p->step_children[j]) {
        GstElement *const tee = gst_element_factory_make ("tee", NULL);
        GstPad *const tee_sink_pad = gst_element_get_static_pad (tee, "sink");
        gst_bin_add (GST_BIN (autoconvert2), tee);
        gst_element_sync_state_with_parent (tee);
        gst_pad_link_full (src_pad, tee_sink_pad, GST_PAD_LINK_CHECK_NOTHING |
            GST_PAD_LINK_CHECK_NO_RECONFIGURE);
        gst_object_unref (tee_sink_pad);
        p->tee_elements[j] = tee;
        gst_object_unref (src_pad);
        src_pad = get_element_pad (tee, "src_%u");
      }
    }

    p->chain_src_pad = src_pad;
  }

  /* Link the chain to the input and output pads. */
  for (it = proposals; it; it = it->next) {
    const struct Proposal *const p = (struct Proposal *) it->data;
    GstElement *const src_tee = p->parent.proposal ?
        p->parent.proposal->tee_elements[p->parent.parent_step] :
        g_hash_table_lookup (sink_pad_tees, GST_PAD (p->parent.pad));
    GstPad *const src_tee_pad = src_tee ?
        get_element_pad (src_tee, "src_%u") : NULL;

    if (src_tee_pad && p->chain_sink_pad) {
      /* Link a source tee to the input of the chain, and the output of the
       * chain to the source ghost pad. */
      g_warn_if_fail (p->chain_sink_pad && p->chain_src_pad);
      gst_pad_link_full (src_tee_pad, p->chain_sink_pad,
          GST_PAD_LINK_CHECK_NOTHING | GST_PAD_LINK_CHECK_NO_RECONFIGURE);
      set_ghost_pad_target_no_reconfigure (GST_GHOST_PAD (p->src_pad),
          p->chain_src_pad);
      gst_object_unref (src_tee_pad);
    } else if (!p->parent.proposal && p->chain_sink_pad) {
      /* Link a sink ghost pad to the input of the chain, and the output of the
       * chain to the source ghost pad. */
      set_ghost_pad_target_no_reconfigure (GST_GHOST_PAD (p->parent.pad),
          p->chain_sink_pad);
      set_ghost_pad_target_no_reconfigure (GST_GHOST_PAD (p->src_pad),
          p->chain_src_pad);
    } else if (src_tee_pad && !p->chain_sink_pad) {
      /* Link a source tee directly through to the source ghost pad. */
      g_warn_if_fail (!p->chain_sink_pad && !p->chain_src_pad);
      set_ghost_pad_target_no_reconfigure (GST_GHOST_PAD (p->src_pad),
          src_tee_pad);
      gst_object_unref (src_tee_pad);
    } else {
      /* All other links are handled elsewhere. */
      g_warn_if_reached ();
    }
  }

  /* Attach fakesinks to all the unused sink pads. */
  for (i = GST_ELEMENT (autoconvert2)->sinkpads; i; i = i->next) {
    GstPad *const pad = (GstPad *) i->data;
    if (!g_hash_table_contains (sink_pad_tees, pad)) {
      GstElement *const fakesink = gst_element_factory_make ("fakesink", NULL);
      gst_bin_add (GST_BIN (autoconvert2), fakesink);
      gst_element_sync_state_with_parent (fakesink);
      set_ghost_pad_target_no_reconfigure (GST_GHOST_PAD (pad),
          get_element_pad (fakesink, "sink"));
    }
  }

  /* Forward the sticky events */
  for (i = GST_ELEMENT (autoconvert2)->sinkpads; i; i = i->next)
    gst_pad_sticky_events_foreach (i->data, forward_sticky_events, NULL);

  /* Tidy up the temporary construction data. */
  for (it = proposals; it; it = it->next) {
    struct Proposal *const p = (struct Proposal *) it->data;
    g_free (p->step_children);
    p->step_children = NULL;
    g_free (p->tee_elements);
    p->tee_elements = NULL;

    if (p->chain_src_pad) {
      gst_object_unref (p->chain_src_pad);
      p->chain_src_pad = NULL;
    }

    if (p->chain_src_pad) {
      gst_object_unref (p->chain_sink_pad);
      p->chain_sink_pad = NULL;
    }
  }

  g_hash_table_destroy (sink_pad_tees);
}

static void
build_graph (GstAutoConvert2 * autoconvert2)
{
  GList *it;

  GSList *const proposals = generate_proposals (autoconvert2);
  GSList *const selected_proposals = select_proposals (autoconvert2, proposals);
  instantiate_proposals (autoconvert2, selected_proposals);
  g_slist_free (selected_proposals);
  g_slist_free_full (proposals, (GDestroyNotify) destroy_proposal);

  for (it = GST_ELEMENT (autoconvert2)->srcpads; it; it = it->next)
    GST_OBJECT_FLAG_UNSET ((GstPad *) it->data, GST_PAD_FLAG_NEED_RECONFIGURE);
}

static void
clear_graph (GstAutoConvert2 * autoconvert2)
{
  GstBin *const bin = GST_BIN (autoconvert2);
  GList *i, *j;

  /* Set the elements into the NULL state. */
  for (i = bin->children; i; i = i->next)
    gst_element_set_state ((GstElement *) i->data, GST_STATE_NULL);

  /* Reset the targets of all ghost pads. */
  for (i = GST_ELEMENT (bin)->pads; i; i = i->next)
    release_ghost_pad (i->data);

  /* Unlink the pads. */
  for (i = bin->children; i; i = i->next) {
    GstElement *const element = (GstElement *) i->data;
    for (j = element->srcpads; j; j = j->next) {
      GstPad *const pad = (GstPad *) j->data;
      GstPad *const peer = gst_pad_get_peer (pad);
      if (peer) {
        gst_pad_unlink (pad, peer);
        release_element_pad (pad);
        release_element_pad (peer);
        gst_object_unref (peer);
      }
    }
  }

  /* Remove the elements from the bin. */
  while (bin->children)
    gst_bin_remove (bin, GST_ELEMENT_CAST (bin->children->data));
}

static void
begin_rebuilding_graph (GstAutoConvert2 * autoconvert2)
{
  GList *it, *sink_pads = NULL;
  gboolean awaiting_drain = FALSE;

  GST_AUTO_CONVERT2_LOCK (autoconvert2);

  enter_build_state (autoconvert2, IDLE, DRAINING_GRAPH);
  g_warn_if_fail (!autoconvert2->priv->pending_drain_pads);

  sink_pads = g_list_copy_deep (GST_ELEMENT (autoconvert2)->sinkpads,
      (GCopyFunc) gst_object_ref, NULL);

  /* Make a list of all the sink pads. */
  autoconvert2->priv->pending_drain_pads = g_hash_table_new (NULL, NULL);
  for (it = GST_ELEMENT (autoconvert2)->srcpads; it; it = it->next)
    g_hash_table_add (autoconvert2->priv->pending_drain_pads, it->data);

  GST_AUTO_CONVERT2_UNLOCK (autoconvert2);

  /* Send EOS events through the graph that will indicate when the graph has
   * drained. */
  for (it = sink_pads; it; it = it->next) {
    GstPad *const pad = gst_ghost_pad_get_target ((GstGhostPad *) it->data);
    if (pad) {
      gst_pad_send_event (pad, gst_event_new_eos ());
      gst_object_unref (pad);
      awaiting_drain = TRUE;
    }
  }

  g_list_free_full (sink_pads, gst_object_unref);

  /* If no EOS events were send, the graph is already drained. */
  if (!awaiting_drain) {
    g_hash_table_destroy (autoconvert2->priv->pending_drain_pads);
    autoconvert2->priv->pending_drain_pads = NULL;
    graph_drained (autoconvert2);
  }
}

static void
graph_drained (GstAutoConvert2 * autoconvert2)
{
  GST_AUTO_CONVERT2_LOCK (autoconvert2);
  enter_build_state (autoconvert2, DRAINING_GRAPH, REBUILDING_GRAPH);

  clear_graph (autoconvert2);
  build_graph (autoconvert2);

  enter_build_state (autoconvert2, REBUILDING_GRAPH, IDLE);
  g_cond_signal (&autoconvert2->priv->sink_block_cond);

  if (autoconvert2->priv->pending_drain_pads) {
    g_hash_table_destroy (autoconvert2->priv->pending_drain_pads);
    autoconvert2->priv->pending_drain_pads = NULL;
  }

  GST_AUTO_CONVERT2_UNLOCK (autoconvert2);
}

static gboolean
needs_reconfigure (GstAutoConvert2 * autoconvert2)
{
  GList *it;
  gboolean ret = FALSE;
  for (it = GST_ELEMENT (autoconvert2)->srcpads; it; it = it->next)
    ret = ret || gst_pad_needs_reconfigure ((GstPad *) it->data);
  return ret;
}
