/*
 *  transform.c
 *
 *  Copyright (C) Georg Martius - June 2007
 *   georg dot martius at web dot de  
 *
 *  This file is part of transcode, a video stream processing tool
 *      
 *  transcode is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  transcode is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
*/

#include "transform.h"

#include "transformfixedpoint.h"
#ifdef TESTING
#include "transformfloat.h"
#endif

#include <math.h>
#include <libgen.h>

// TODO: apply crop-fix patch

const char* interpolTypes[5] = {"No (0)", "Linear (1)", "Bi-Linear (2)", 
                                "Bi-Cubic (3)"};


int initTransformData(TransformData* td, const DSFrameInfo* fi_src, 
                      const DSFrameInfo* fi_dest , const char* modName){
    td->modName = modName;
    
    td->fiSrc = *fi_src;
    td->fiDest = *fi_dest;   
    
    td->src = ds_zalloc(td->fiSrc.framesize); /* FIXME */
    if (td->src == NULL) {
        ds_log_error(td->modName, "tc_malloc failed\n");
        return DS_ERROR;
    }

    td->dest = 0;
    
    /* Options */
    td->maxShift = -1;
    td->maxAngle = -1;

    td->crop = 0;
    td->relative = 1;
    td->invert = 0;
    td->smoothing = 10;
  
    td->rotationThreshhold = 0.25/(180/M_PI);

    td->zoom    = 0;
    td->optZoom = 1;
    td->interpolType = BiLinear;
    td->sharpen = 0.8;  

    td->verbose = 0;
    return DS_OK;
}

int configureTransformData(TransformData* td){
    if (td->maxShift > td->fiDest.width/2) 
        td->maxShift = td->fiDest.width/2;
    if (td->maxShift > td->fiDest.height/2)
        td->maxShift = td->fiDest.height/2;
    
    td->interpolType = DS_MIN(td->interpolType,BiCubic);

    switch(td->interpolType){
      case Zero:     interpolate = &interpolateZero; break;
      case Linear:   interpolate = &interpolateLin; break;
      case BiLinear: interpolate = &interpolateBiLin; break;
      case BiCubic:  interpolate = &interpolateBiCub; break;
      default: interpolate = &interpolateBiLin;
    }
#ifdef TESTING
    switch(td->interpolType){
      case Zero:     interpolate_float = &interpolateZero_float; break;
      case Linear:   interpolate_float = &interpolateLin_float; break;
      case BiLinear: interpolate_float = &interpolateBiLin_float; break;
      case BiCubic:  interpolate_float = &interpolateBiCub_float; break;
      default: interpolate_float = &interpolateBiLin_float;
    }

#endif
    return DS_OK;
}

void cleanupTransformData(TransformData* td){
    if (td->src) {
        ds_free(td->src);
        td->src = NULL;
    }
}

int transformPrepare(TransformData* td, unsigned char* frame_buf){
    td->dest = frame_buf;
    // we first copy the frame to the src and then overwrite the destination
    // with the transformed version.
    memcpy(td->src,  frame_buf, td->fiSrc.framesize);
    return DS_OK;   
}
  

Transform getNextTransform(const TransformData* td, Transformations* trans){
    if (trans->current >= trans->len) {        
        trans->current = trans->len-1;
        if(!trans->warned_end)
            ds_log_warn(td->modName, "not enough transforms found, use last transformation!\n");
        trans->warned_end = 1;                 
    }    
    trans->current++;
    return trans->ts[trans->current-1];
}

void initTransformations(Transformations* trans){
    trans->ts = 0;
    trans->len = 0;
    trans->current = 0;
    trans->warned_end = 0;  
}

void cleanupTransformations(Transformations* trans){
    if (trans->ts) {
        ds_free(trans->ts);
        trans->ts = NULL;        
    }
    trans->len=0;
}

/** 
 * read_transforms: read transforms file
 *  The format is as follows:
 *   Lines with # at the beginning are comments and will be ignored
 *   Data lines have 5 columns seperated by space or tab containing
 *   time, x-translation, y-translation, alpha-rotation, extra
 *   where time and extra are integers 
 *   and the latter is unused at the moment
 *
 * Parameters:
 *         f:  file description
 *         trans: place to store the transforms
 * Return value: 
 *         number of transforms read
 * Preconditions: f is opened
 */
int readTransforms(const TransformData* td, FILE* f , Transformations* trans)
{
    char l[1024];
    int s = 0;
    int i = 0;
    int ti; // time (ignored)
    Transform t;
    
    while (fgets(l, sizeof(l), f)) {
        if (l[0] == '#')
            continue;    //  ignore comments
        if (strlen(l) == 0)
            continue; //  ignore empty lines
        // try new format
        if (sscanf(l, "%i %lf %lf %lf %lf %i", &ti, &t.x, &t.y, &t.alpha, 
                   &t.zoom, &t.extra) != 6) {
            if (sscanf(l, "%i %lf %lf %lf %i", &ti, &t.x, &t.y, &t.alpha, 
                       &t.extra) != 5) {                
                ds_log_error(td->modName, "Cannot parse line: %s", l);
                return 0;
            }
            t.zoom=0;
        }
    
        if (i>=s) { // resize transform array
            if (s == 0)
                s = 256;
            else
                s*=2;
            /* ds_log_info(td->modName, "resize: %i\n", s); */
            trans->ts = ds_realloc(trans->ts, sizeof(Transform)* s);
            if (!trans->ts) {
                ds_log_error(td->modName, "Cannot allocate memory"
                                       " for transformations: %i\n", s);
                return 0;
            }
        }
        trans->ts[i] = t;
        i++;
    }
    trans->len = i;

    return i;
}

/**
 * preprocess_transforms: does smoothing, relative to absolute conversion,
 *  and cropping of too large transforms.
 *  This is actually the core algorithm for canceling the jiggle in the 
 *  movie. We perform a low-pass filter in terms of transformation size.
 *  This enables still camera movement, but in a smooth fasion.
 *
 * Parameters:
 *            td: tranform private data structure
 * Return value:
 *     1 for success and 0 for failture
 * Preconditions:
 *     None
 * Side effects:
 *     td->trans will be modified
 */
int preprocessTransforms(TransformData* td, Transformations* trans)
{
    Transform* ts = trans->ts;
    int i;

    if (trans->len < 1)
        return 0;
    if (td->verbose & DS_DEBUG) {
        ds_log_msg(td->modName, "Preprocess transforms:");
    }
    if (td->smoothing>0) {
        /* smoothing */
        Transform* ts2 = ds_malloc(sizeof(Transform) * trans->len);
        memcpy(ts2, ts, sizeof(Transform) * trans->len);

        /*  we will do a sliding average with minimal update
         *   \hat x_{n/2} = x_1+x_2 + .. + x_n
         *   \hat x_{n/2+1} = x_2+x_3 + .. + x_{n+1} = x_{n/2} - x_1 + x_{n+1}
         *   avg = \hat x / n
         */
        int s = td->smoothing * 2 + 1;
        Transform null = null_transform();
        /* avg is the average over [-smoothing, smoothing] transforms 
           around the current point */
        Transform avg;
        /* avg2 is a sliding average over the filtered signal! (only to past) 
         *  with smoothing * 10 horizont to kill offsets */
        Transform avg2 = null_transform();
        double tau = 1.0/(3 * s);
        /* initialise sliding sum with hypothetic sum centered around
         * -1st element. We have two choices:
         * a) assume the camera is not moving at the beginning 
         * b) assume that the camera moves and we use the first transforms
         */
        Transform s_sum = null; 
        for (i = 0; i < td->smoothing; i++){
            s_sum = add_transforms(&s_sum, i < trans->len ? &ts2[i]:&null);
        }
        mult_transform(&s_sum, 2); // choice b (comment out for choice a)

        for (i = 0; i < trans->len; i++) {
            Transform* old = ((i - td->smoothing - 1) < 0) 
                ? &null : &ts2[(i - td->smoothing - 1)];
            Transform* new = ((i + td->smoothing) >= trans->len) 
                ? &null : &ts2[(i + td->smoothing)];
            s_sum = sub_transforms(&s_sum, old);
            s_sum = add_transforms(&s_sum, new);

            avg = mult_transform(&s_sum, 1.0/s);

            /* lowpass filter: 
             * meaning high frequency must be transformed away
             */
            ts[i] = sub_transforms(&ts2[i], &avg);
            /* kill accumulating offset in the filtered signal*/
            avg2 = add_transforms_(mult_transform(&avg2, 1 - tau),
                                   mult_transform(&ts[i], tau));
            ts[i] = sub_transforms(&ts[i], &avg2);

            if (td->verbose & DS_DEBUG) {
                ds_log_msg(td->modName, 
                           "s_sum: %5lf %5lf %5lf, ts: %5lf, %5lf, %5lf\n", 
                           s_sum.x, s_sum.y, s_sum.alpha, 
                           ts[i].x, ts[i].y, ts[i].alpha);
                ds_log_msg(td->modName, 
                           "  avg: %5lf, %5lf, %5lf avg2: %5lf, %5lf, %5lf", 
                           avg.x, avg.y, avg.alpha, 
                           avg2.x, avg2.y, avg2.alpha);      
            }
        }
        ds_free(ts2);
    }
  
  
    /*  invert? */
    if (td->invert) {
        for (i = 0; i < trans->len; i++) {
            ts[i] = mult_transform(&ts[i], -1);      
        }
    }
  
    /* relative to absolute */
    if (td->relative) {
        Transform t = ts[0];
        for (i = 1; i < trans->len; i++) {
            if (td->verbose  & DS_DEBUG) {
                ds_log_msg(td->modName, "shift: %5lf   %5lf   %lf \n", 
                           t.x, t.y, t.alpha *180/M_PI);
            }
            ts[i] = add_transforms(&ts[i], &t); 
            t = ts[i];
        }
    }
    /* crop at maximal shift */
    if (td->maxShift != -1)
        for (i = 0; i < trans->len; i++) {
            ts[i].x     = DS_CLAMP(ts[i].x, -td->maxShift, td->maxShift);
            ts[i].y     = DS_CLAMP(ts[i].y, -td->maxShift, td->maxShift);
        }
    if (td->maxAngle != - 1.0)
        for (i = 0; i < trans->len; i++)
            ts[i].alpha = DS_CLAMP(ts[i].alpha, -td->maxAngle, td->maxAngle);

    /* Calc optimal zoom 
     *  cheap algo is to only consider transformations
     *  uses cleaned max and min 
     * Todo: use sliding average to zoom only as much as needed. 
     *       use also rotation angles (transform all four corners)
     *       optzoom=2?
     */
    if (td->optZoom != 0 && trans->len > 1){    
        Transform min_t, max_t;
        cleanmaxmin_xy_transform(ts, trans->len, 10, &min_t, &max_t); 
        // the zoom value only for x
        double zx = 2*DS_MAX(max_t.x,fabs(min_t.x))/td->fiSrc.width;
        // the zoom value only for y
        double zy = 2*DS_MAX(max_t.y,fabs(min_t.y))/td->fiSrc.height;
        td->zoom += 100* DS_MAX(zx,zy); // use maximum
        ds_log_info(td->modName, "Final zoom: %lf\n", td->zoom);
    }
        
    /* apply global zoom */
    if (td->zoom != 0){
        for (i = 0; i < trans->len; i++)
            ts[i].zoom += td->zoom;       
    }

    return DS_OK;
}


/*
 * Local variables:
 *   c-file-style: "stroustrup"
 *   c-file-offsets: ((case-label . *) (statement-case-intro . *))
 *   indent-tabs-mode: nil
 * End:
 *
 * vim: expandtab shiftwidth=4:
 */
