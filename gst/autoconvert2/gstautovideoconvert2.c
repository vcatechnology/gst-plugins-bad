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

#include "gstautovideoconvert2.h"

GST_DEBUG_CATEGORY (autovideoconvert2_debug);
#define GST_CAT_DEFAULT (autovideoconvert2_debug)

G_DEFINE_TYPE (GstAutoVideoConvert2, gst_auto_video_convert2,
    GST_TYPE_AUTO_CONVERT2);

static void
gst_auto_video_convert2_class_init (GstAutoVideoConvert2Class * klass)
{
  GST_DEBUG_CATEGORY_INIT (autovideoconvert2_debug, "autovideoconvert2", 0,
      "autovideoconvert2 element");
}

static void
gst_auto_video_convert2_init (GstAutoVideoConvert2 * autovideoconvert2)
{
}
