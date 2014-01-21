/*
 * filter_vidstab.cpp
 * Copyright (C) 2013 Marco Gittler <g.marco@freenet.de>
 * Copyright (C) 2013 Jakub Ksiezniak <j.ksiezniak@gmail.com>
 * Copyright (C) 2014 Brian Matherly <pez4brian@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

extern "C"
{
#include <vid.stab/libvidstab.h>
#include <framework/mlt.h>
#include <framework/mlt_animation.h>
#include "common.h"
}

#include <sstream>
#include <string.h>
#include <assert.h>

typedef struct
{
	VSMotionDetect md;
	VSManyLocalMotions mlms;
	mlt_position last_position;
} vs_analyze;

typedef struct
{
	VSTransformData td;
	VSTransformConfig conf;
	VSTransformations trans;
} vs_apply;

typedef struct
{
	vs_analyze* analyze_data;
	vs_apply* apply_data;
} vs_data;

/** Free all data used by a VSManyLocalMotions instance.
 */

static void free_manylocalmotions( VSManyLocalMotions* mlms )
{
	for( int i = 0; i < vs_vector_size( mlms ); i++ )
	{
		LocalMotions* lms = (LocalMotions*)vs_vector_get( mlms, i );

		if( lms )
		{
			vs_vector_del( lms );
		}
	}
	vs_vector_del( mlms );
}

/** Serialize a VSManyLocalMotions instance and store it in the properties.
 *
 * Each LocalMotions instance is converted to a string and stored in an animation.
 * Then, the entire animation is serialized and stored in the properties.
 * \param properties the filter properties
 * \param mlms an initialized VSManyLocalMotions instance
 */

static void publish_manylocalmotions( mlt_properties properties, VSManyLocalMotions* mlms )
{
	mlt_animation animation = mlt_animation_new();
	mlt_animation_item_s item;

	// Initialize animation item.
	item.is_key = 1;
	item.keyframe_type = mlt_keyframe_discrete;
	item.property = mlt_property_init();

	// Convert each LocalMotions instance to a string and add it to the animation.
	for( int i = 0; i < vs_vector_size( mlms ); i++ )
	{
		LocalMotions* lms = (LocalMotions*)vs_vector_get( mlms, i );
		item.frame = i;
		int size = vs_vector_size( lms );

		std::ostringstream oss;
		for ( int j = 0; j < size; ++j )
		{
			LocalMotion* lm = (LocalMotion*)vs_vector_get( lms, j );
			oss << lm->v.x << ' ';
			oss << lm->v.y << ' ';
			oss << lm->f.x << ' ';
			oss << lm->f.y << ' ';
			oss << lm->f.size << ' ';
			oss << lm->contrast << ' ';
			oss << lm->match << ' ';
		}
		mlt_property_set_string( item.property, oss.str().c_str() );
		mlt_animation_insert( animation, &item );
	}

	// Serialize and store the animation.
	char* motion_str = mlt_animation_serialize( animation );
	mlt_properties_set( properties, "results", motion_str );

	mlt_property_close( item.property );
	mlt_animation_close( animation );
	free( motion_str );
}

/** Get the motions data from the properties and convert it to a VSManyLocalMotions.
 *
 * Each LocalMotions instance is converted to a string and stored in an animation.
 * Then, the entire animation is serialized and stored in the properties.
 * \param properties the filter properties
 * \param mlms an initialized (but empty) VSManyLocalMotions instance
 */

static void read_manylocalmotions( mlt_properties properties, VSManyLocalMotions* mlms )
{
	mlt_animation_item_s item;
	item.property = mlt_property_init();
	mlt_animation animation = mlt_animation_new();
	// Get the results property which represents the VSManyLocalMotions
	char* motion_str = mlt_properties_get( properties, "results" );

	mlt_animation_parse( animation, motion_str, 0, 0, NULL );

	int length = mlt_animation_get_length( animation );

	for ( int i = 0; i <= length; ++i )
	{
		LocalMotions lms;
		vs_vector_init( &lms, 0 );

		// Get the animation item that represents the LocalMotions
		mlt_animation_get_item( animation, &item, i );

		// Convert the property to a real LocalMotions
		std::istringstream iss( mlt_property_get_string( item.property ) );
		while ( iss.good() )
		{
			LocalMotion lm;
			iss >> lm.v.x >> lm.v.y >> lm.f.x >> lm.f.y >> lm.f.size >> lm.contrast >> lm.match;
			if ( !iss.fail() )
			{
				vs_vector_append_dup( &lms, &lm, sizeof(lm) );
			}
		}

		// Add the LocalMotions to the ManyLocalMotions
		vs_vector_set_dup( mlms, i, &lms, sizeof(LocalMotions) );
	}

	mlt_property_close( item.property );
	mlt_animation_close( animation );
}

static void get_transform_config( VSTransformConfig* conf, mlt_filter filter, mlt_frame frame )
{
	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
	const char* filterName = mlt_properties_get( properties, "mlt_service" );

	*conf = vsTransformGetDefaultConfig( filterName );
	conf->smoothing = mlt_properties_get_int( properties, "smoothing" );
	conf->maxShift = mlt_properties_get_int( properties, "maxshift" );
	conf->maxAngle = mlt_properties_get_double( properties, "maxangle" );
	conf->crop = (VSBorderType)mlt_properties_get_int( properties, "crop" );
	conf->zoom = mlt_properties_get_int( properties, "zoom" );
	conf->optZoom = mlt_properties_get_int( properties, "optzoom" );
	conf->zoomSpeed = mlt_properties_get_double( properties, "zoomspeed" );
	conf->relative = mlt_properties_get_int( properties, "relative" );
	conf->invert = mlt_properties_get_int( properties, "invert" );
	if ( mlt_properties_get_int( properties, "tripod" ) != 0 )
	{
		// Virtual tripod mode: relative=False, smoothing=0
		conf->relative = 0;
		conf->smoothing = 0;
	}

	// by default a bicubic interpolation is selected
	const char *interps = mlt_properties_get( MLT_FRAME_PROPERTIES( frame ), "rescale.interp" );
	conf->interpolType = VS_BiCubic;
	if ( strcmp( interps, "nearest" ) == 0 || strcmp( interps, "neighbor" ) == 0 )
		conf->interpolType = VS_Zero;
	else if ( strcmp( interps, "tiles" ) == 0 || strcmp( interps, "fast_bilinear" ) == 0 )
		conf->interpolType = VS_Linear;
	else if ( strcmp( interps, "bilinear" ) == 0 )
		conf->interpolType = VS_BiLinear;
}

static int check_apply_config( mlt_filter filter, mlt_frame frame )
{
	vs_apply* apply_data = ((vs_data*)filter->child)->apply_data;

	if( apply_data )
	{
		VSTransformConfig new_conf;
		memset( &new_conf, 0, sizeof(VSTransformConfig) );
		get_transform_config( &new_conf, filter, frame );
		return compare_transform_config( &apply_data->conf, &new_conf );
	}

	return 0;
}

static void init_apply_data( mlt_filter filter, mlt_frame frame, VSPixelFormat vs_format, int width, int height )
{
	vs_data* data = (vs_data*)filter->child;
	vs_apply* apply_data = (vs_apply*)calloc( 1, sizeof(vs_apply) );
	memset( apply_data, 0, sizeof( vs_apply ) );

	// Initialize the VSTransformConfig
	get_transform_config( &apply_data->conf, filter, frame );

	// Initialize VSTransformData
	VSFrameInfo fi_src, fi_dst;
	vsFrameInfoInit( &fi_src, width, height, vs_format );
	vsFrameInfoInit( &fi_dst, width, height, vs_format );
	vsTransformDataInit( &apply_data->td, &apply_data->conf, &fi_src, &fi_dst );

	// Initialize VSTransformations
	vsTransformationsInit( &apply_data->trans );

	// Load the motions from the analyze step and convert them to VSTransformations
	VSManyLocalMotions mlms;
	vs_vector_init( &mlms, mlt_filter_get_length2( filter, frame ) );
	read_manylocalmotions( MLT_FILTER_PROPERTIES( filter ), &mlms );
	vsLocalmotions2Transforms( &apply_data->td, &mlms, &apply_data->trans );
	vsPreprocessTransforms( &apply_data->td, &apply_data->trans );
	free_manylocalmotions( &mlms );

	data->apply_data = apply_data;
}

static void destory_apply_data( vs_apply* apply_data )
{
	if ( apply_data )
	{
		vsTransformDataCleanup( &apply_data->td );
		vsTransformationsCleanup( &apply_data->trans );
		free( apply_data );
	}
}

static void init_analyze_data( mlt_filter filter, mlt_frame frame, VSPixelFormat vs_format, int width, int height )
{
	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
	vs_data* data = (vs_data*)filter->child;
	vs_analyze* analyze_data = (vs_analyze*)calloc( 1, sizeof(vs_analyze) );
	memset( analyze_data, 0, sizeof(vs_analyze) );

	// Initialize the saved VSManyLocalMotions vector where motion data will be
	// stored for each frame.
	vs_vector_init( &analyze_data->mlms, mlt_filter_get_length2( filter, frame ) );

	// Initialize a VSMotionDetectConfig
	const char* filterName = mlt_properties_get( properties, "mlt_service" );
	VSMotionDetectConfig conf = vsMotionDetectGetDefaultConfig( filterName );
	conf.shakiness = mlt_properties_get_int( properties, "shakiness" );
	conf.accuracy = mlt_properties_get_int( properties, "accuracy" );
	conf.stepSize = mlt_properties_get_int( properties, "stepsize" );
	conf.contrastThreshold = mlt_properties_get_double( properties, "mincontrast" );
	conf.show = mlt_properties_get_int( properties, "show" );
	conf.virtualTripod = mlt_properties_get_int( properties, "tripod" );

	// Initialize a VSFrameInfo
	VSFrameInfo fi;
	vsFrameInfoInit( &fi, width, height, vs_format );

	// Initialize the saved VSMotionDetect
	vsMotionDetectInit( &analyze_data->md, &conf, &fi );

	data->analyze_data = analyze_data;
}

void destory_analyze_data( vs_analyze* analyze_data )
{
	if ( analyze_data )
	{
		vsMotionDetectionCleanup( &analyze_data->md );
		free_manylocalmotions( &analyze_data->mlms );
		free( analyze_data );
	}
}

static int apply_results( mlt_filter filter, mlt_frame frame, uint8_t* vs_image, VSPixelFormat vs_format, int width, int height )
{
	int error = 0;
	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
	vs_data* data = (vs_data*)filter->child;

	if ( check_apply_config( filter, frame ) ||
		mlt_properties_get_int( properties, "reload" ) )
	{
		mlt_properties_set_int( properties, "reload", 0 );
		destory_apply_data( data->apply_data );
		data->apply_data = NULL;
	}

	// Init transform data if necessary (first time)
	if ( !data->apply_data )
	{
		init_apply_data( filter, frame, vs_format, width, height );
	}

	// Apply transformations to this image
	VSTransformData* td = &data->apply_data->td;
	VSTransformations* trans = &data->apply_data->trans;
	VSFrame vsFrame;
	vsFrameFillFromBuffer( &vsFrame, vs_image, vsTransformGetSrcFrameInfo( td ) );
	trans->current = mlt_filter_get_position( filter, frame );
	vsTransformPrepare( td, &vsFrame, &vsFrame );
	VSTransform t = vsGetNextTransform( td, trans );
	vsDoTransform( td, t );
	vsTransformFinish( td );

	return error;
}

static void analyze_image( mlt_filter filter, mlt_frame frame, uint8_t* vs_image, VSPixelFormat vs_format, int width, int height )
{
	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
	vs_data* data = (vs_data*)filter->child;
	mlt_position pos = mlt_filter_get_position( filter, frame );

	// If any frames are skipped, analysis data will be incomplete.
	if( data->analyze_data && pos != data->analyze_data->last_position + 1 )
	{
		destory_analyze_data( data->analyze_data );
		data->analyze_data = NULL;
	}

	if ( !data->analyze_data && pos == 0 )
	{
		// Analysis must start on the first frame
		init_analyze_data( filter, frame, vs_format, width, height );
	}

	if( data->analyze_data )
	{
		// Initialize the VSFrame to be analyzed.
		VSMotionDetect* md = &data->analyze_data->md;
		LocalMotions localmotions;
		VSFrame vsFrame;
		vsFrameFillFromBuffer( &vsFrame, vs_image, &md->fi );

		// Detect and save motions.
		vsMotionDetection( md, &localmotions, &vsFrame );
		vs_vector_set_dup( &data->analyze_data->mlms, pos, &localmotions, sizeof(LocalMotions) );

		// Publish the motions if this is the last frame.
		if ( pos + 1 == mlt_filter_get_length2( filter, frame ) )
		{
			publish_manylocalmotions( properties, &data->analyze_data->mlms );
		}

		data->analyze_data->last_position = pos;
	}
}

static int get_image( mlt_frame frame, uint8_t **image, mlt_image_format *format, int *width, int *height, int writable )
{
	mlt_filter filter = (mlt_filter)mlt_frame_pop_service( frame );
	mlt_properties properties = MLT_FILTER_PROPERTIES( filter );
	uint8_t* vs_image = NULL;
	VSPixelFormat vs_format = PF_NONE;

	// VS only works on progressive frames
	mlt_properties_set_int( MLT_FRAME_PROPERTIES( frame ), "consumer_deinterlace", 1 );

	*format = validate_format( *format );

	int error = mlt_frame_get_image( frame, image, format, width, height, 1 );

	// Convert the received image to a format vid.stab can handle
	if ( !error )
	{
		vs_format = mltimage_to_vsimage( *format, *width, *height, *image, &vs_image );
	}

	if( vs_image )
	{
		mlt_service_lock( MLT_FILTER_SERVICE(filter) );

		char* results = mlt_properties_get( properties, "results" );
		if( results && strcmp( results, "" ) )
		{
			apply_results( filter, frame, vs_image, vs_format, *width, *height );
			vsimage_to_mltimage( vs_image, *image, *format, *width, *height );
		}
		else
		{
			analyze_image( filter, frame, vs_image, vs_format, *width, *height );
			if( mlt_properties_get_int( properties, "show" ) == 1 )
			{
				vsimage_to_mltimage( vs_image, *image, *format, *width, *height );
			}
		}

		mlt_service_unlock( MLT_FILTER_SERVICE(filter) );

		free_vsimage( vs_image, vs_format );
	}

	return error;
}

static mlt_frame process_filter( mlt_filter filter, mlt_frame frame )
{
	mlt_frame_push_service( frame, filter );
	mlt_frame_push_get_image( frame, get_image );
	return frame;
}

static void filter_close( mlt_filter filter )
{
	vs_data* data = (vs_data*)filter->child;
	if ( data )
	{
		if ( data->analyze_data ) destory_analyze_data( data->analyze_data );
		if ( data->apply_data ) destory_apply_data( data->apply_data );
		free( data );
	}
	filter->close = NULL;
	filter->child = NULL;
	filter->parent.close = NULL;
	mlt_service_close( &filter->parent );
}

extern "C"
{

mlt_filter filter_vidstab_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg )
{
	mlt_filter filter = mlt_filter_new();
	vs_data* data = (vs_data*)calloc( 1, sizeof(vs_data) );

	if ( filter && data )
	{
		data->analyze_data = NULL;
		data->apply_data = NULL;

		filter->close = filter_close;
		filter->child = data;
		filter->process = process_filter;

		mlt_properties properties = MLT_FILTER_PROPERTIES(filter);

		//properties for analyze
		mlt_properties_set(properties, "shakiness", "4");
		mlt_properties_set(properties, "accuracy", "4");
		mlt_properties_set(properties, "stepsize", "6");
		mlt_properties_set(properties, "algo", "1");
		mlt_properties_set(properties, "mincontrast", "0.3");
		mlt_properties_set(properties, "show", "0");
		mlt_properties_set(properties, "tripod", "0");

		// properties for apply
		mlt_properties_set(properties, "smoothing", "15");
		mlt_properties_set(properties, "maxshift", "-1");
		mlt_properties_set(properties, "maxangle", "-1");
		mlt_properties_set(properties, "crop", "0");
		mlt_properties_set(properties, "invert", "0");
		mlt_properties_set(properties, "relative", "1");
		mlt_properties_set(properties, "zoom", "0");
		mlt_properties_set(properties, "optzoom", "1");
		mlt_properties_set(properties, "zoomspeed", "0.25");
		mlt_properties_set( properties, "reload", "0" );

		mlt_properties_set(properties, "vid.stab.version", LIBVIDSTAB_VERSION);
	}
	else
	{
		if( filter )
		{
			mlt_filter_close( filter );
		}

		if( data )
		{
			free( data );
		}

		filter = NULL;
	}
	return filter;
}

}