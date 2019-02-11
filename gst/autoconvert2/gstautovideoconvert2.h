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

#ifndef __GST_AUTO_VIDEO_CONVERT2_H__
#define __GST_AUTO_VIDEO_CONVERT2_H__

#include <gst/gst.h>
#include "gstautoconvert2.h"

G_BEGIN_DECLS
#define GST_TYPE_AUTO_VIDEO_CONVERT2 	        	(gst_auto_video_convert2_get_type())
#define GST_AUTO_VIDEO_CONVERT2(obj)	            (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_AUTO_VIDEO_CONVERT2,GstAutoVideoConvert2))
#define GST_AUTO_VIDEO_CONVERT2_CLASS(klass)       (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_AUTO_VIDEO_CONVERT2,GstAutoVideoConvert2Class))
#define GST_IS_AUTO_VIDEO_CONVERT2(obj)            (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_AUTO_VIDEO_CONVERT2))
#define GST_IS_AUTO_VIDEO_CONVERT2_CLASS(klass)     (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_AUTO_VIDEO_CONVERT2))
typedef struct _GstAutoVideoConvert2 GstAutoVideoConvert2;
typedef struct _GstAutoVideoConvert2Class GstAutoVideoConvert2Class;

typedef struct _GstAutoVideoConvert2Priv GstAutoVideoConvert2Priv;

struct _GstAutoVideoConvert2
{
  /*< private > */
  GstAutoConvert2 base;         /* we extend GstAutoConvert2 */

  /* Internal private data. */
  GstAutoVideoConvert2Priv *priv;
};

struct _GstAutoVideoConvert2Class
{
  GstAutoConvert2Class parent_class;
};

GType gst_auto_video_convert2_get_type (void);

G_END_DECLS
#endif /* __GST_AUTO_VIDEO_CONVERT2_H__ */

