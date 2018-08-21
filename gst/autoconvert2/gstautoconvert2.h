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


#ifndef __GST_AUTO_CONVERT2_H__
#define __GST_AUTO_CONVERT2_H__

#include <gst/gst.h>
#include <gst/gstbin.h>

G_BEGIN_DECLS
#define GST_TYPE_AUTO_CONVERT2 	        	(gst_auto_convert2_get_type())
#define GST_AUTO_CONVERT2(obj)	                (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AUTO_CONVERT2,GstAutoConvert2))
#define GST_AUTO_CONVERT2_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AUTO_CONVERT2,GstAutoConvert2Class))
#define GST_AUTO_CONVERT2_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS((obj) ,AUTO_CONVERT2,GstAutoConvert2Class))
#define GST_IS_AUTO_CONVERT2(obj)              (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AUTO_CONVERT2))
#define GST_IS_AUTO_CONVERT2_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AUTO_CONVERT2))
typedef struct _GstAutoConvert2 GstAutoConvert2;
typedef struct _GstAutoConvert2Class GstAutoConvert2Class;

typedef struct _GstAutoConvert2Priv GstAutoConvert2Priv;

typedef struct
{
  struct {
    GstPad *pad;
    GstCaps *caps;
  } sink, src;
} GstAutoConvert2TransformRoute;

typedef struct
{
  GstElementFactory *factory;
  GstStaticPadTemplate *sink_pad_template, *src_pad_template;
  GstCaps *sink_caps, *src_caps;
} GstAutoConvert2TransformationStep;

struct _GstAutoConvert2
{
  GstBin bin;                   /* we extend GstBin */

  /* Internal private data. */
  GstAutoConvert2Priv *priv;
};

struct _GstAutoConvert2Class
{
  GstBinClass parent_class;

  GList* (*get_factories) (GstAutoConvert2 * autoconvert2);
  gboolean (*validate_transform_route) (GstAutoConvert2 * autoconvert2,
    const GstAutoConvert2TransformRoute * route);
  int (*validate_chain) (GstAutoConvert2 * autoconvert2,
    GstCaps *chain_sink_caps, GstCaps *chain_src_caps, GSList **chain,
    guint chain_length);
  guint (*cost_transformation_step) (GstAutoConvert2 * autoconvert2,
    const GstAutoConvert2TransformationStep * transformation_step);
  void (*begin_building_graph) (GstAutoConvert2 * autoconvert2);
};

GType gst_auto_convert2_get_type (void);

G_END_DECLS
#endif /* __GST_AUTO_CONVERT2_H__ */
