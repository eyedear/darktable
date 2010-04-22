/*
	 This file is part of darktable,
	 copyright (c) 2010 Henrik Andersson.

	 darktable is free software: you can redistribute it and/or modify
	 it under the terms of the GNU General Public License as published by
	 the Free Software Foundation, either version 3 of the License, or
	 (at your option) any later version.

	 darktable is distributed in the hope that it will be useful,
	 but WITHOUT ANY WARRANTY; without even the implied warranty of
	 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	 GNU General Public License for more details.

	 You should have received a copy of the GNU General Public License
	 along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif
#include <unistd.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include "common/camera_control.h"


/** Initializes camera */
gboolean _camera_initialize(const dt_camctl_t *c, dt_camera_t *cam);

/** Poll camera events, this one is called from the thread handling the camera. */
void _camera_poll_events(const dt_camctl_t *c,const dt_camera_t *cam);


void _camctl_lock(const dt_camctl_t *c,const dt_camera_t *cam);
void _camctl_unlock(const dt_camctl_t *c);

/** Dispatch functions for listener interfaces */
const char *_dispatch_request_image_path(const dt_camctl_t *c,const dt_camera_t *camera);
const char *_dispatch_request_image_filename(const dt_camctl_t *c,const char *filename,const dt_camera_t *camera);
void _dispatch_camera_image_downloaded(const dt_camctl_t *c,const dt_camera_t *camera,const char *filename);
void _dispatch_camera_connected(const dt_camctl_t *c,const dt_camera_t *camera);
void _dispatch_camera_disconnected(const dt_camctl_t *c,const dt_camera_t *camera);
void _dispatch_control_status(const dt_camctl_t *c,dt_camctl_status_t status);
void _dispatch_camera_error(const dt_camctl_t *c,const dt_camera_t *camera,dt_camera_error_t error);
void _dispatch_camera_storage_image_filename(const dt_camctl_t *c,const dt_camera_t *camera,const char *filename,CameraFile *preview);

static void _idle_func_dispatch(GPContext *context, void *data) {
	gdk_threads_enter();
	if( gtk_events_pending () ) gtk_main_iteration();
	gdk_threads_leave();
}

static void _error_func_dispatch(GPContext *context, const char *format, va_list args, void *data) {
	dt_camctl_t *camctl=(dt_camctl_t *)data;
	char buffer[4096];
	vsprintf( buffer, format, args );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] gphoto2 error: %s\n",buffer);
	
	if(strstr(buffer,"PTP"))
		_dispatch_camera_error(camctl,camctl->active_camera,CAMERA_CONNECTION_BROKEN);
}

static void _status_func_dispatch(GPContext *context, const char *format, va_list args, void *data) {
	char buffer[4096];
	vsprintf( buffer, format, args );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] gphoto2 status: %s\n",buffer);
}

static void _message_func_dispatch(GPContext *context, const char *format, va_list args, void *data) {
	char buffer[4096];
	vsprintf( buffer, format, args );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] gphoto2 message: %s\n",buffer);
}



void _camctl_lock(const dt_camctl_t *c,const dt_camera_t *cam) 
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	pthread_mutex_lock(&camctl->mutex);
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Camera control locked for camera %lx\n",(unsigned long int)cam);
	camctl->active_camera=cam;
	_dispatch_control_status(c,CAMERA_CONTROL_BUSY);
}

void _camctl_unlock(const dt_camctl_t *c)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	const dt_camera_t *cam=camctl->active_camera;
	camctl->active_camera=NULL;
	pthread_mutex_unlock(&camctl->mutex);
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Camera control un-locked for camera %lx\n",(unsigned long int)cam);
	_dispatch_control_status(c,CAMERA_CONTROL_AVAILABLE);
}

dt_camctl_t *dt_camctl_new()
{
	dt_camctl_t *camctl=g_malloc(sizeof(dt_camctl_t));
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Creating new context %lx\n",(unsigned long int)camctl);

	// Initialize gphoto2 context and setup dispatch callbacks
	camctl->gpcontext = gp_context_new();
	gp_context_set_idle_func( camctl->gpcontext , _idle_func_dispatch, camctl );
	gp_context_set_status_func( camctl->gpcontext , _status_func_dispatch, camctl );
	gp_context_set_error_func( camctl->gpcontext , _error_func_dispatch, camctl );
	gp_context_set_message_func( camctl->gpcontext , _message_func_dispatch, camctl );
	
	gp_port_info_list_new( &camctl->gpports );
	gp_abilities_list_new( &camctl->gpcams );

	// Load drivers
	gp_port_info_list_load( camctl->gpports );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Loaded %d port drivers.\n", gp_port_info_list_count( camctl->gpports ) );	
	
	// Load all camera drivers we know...
	gp_abilities_list_load( camctl->gpcams, camctl->gpcontext );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Loaded %d camera drivers.\n", gp_abilities_list_count( camctl->gpcams ) );	
	
	pthread_mutex_init(&camctl->mutex, NULL);
	//pthread_create(&camctl->thread, NULL, &_camera_control_thread, camctl);
	
	// Let's detect cameras connexted
	dt_camctl_detect_cameras(camctl);
	
	return camctl;
}

void dt_camctl_destroy(const dt_camctl_t *c)
{
	// TODO: Go thru all c->cameras and free them..
	
}

void dt_camctl_register_listener( const dt_camctl_t *c, dt_camctl_listener_t *listener)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	pthread_mutex_lock(&camctl->mutex);
	if( g_list_find(camctl->listeners,listener) == NULL )
	{
		camctl->listeners=g_list_append(camctl->listeners,listener);
		dt_print(DT_DEBUG_CAMCTL,"[camera_control] Registering listener %lx\n",(unsigned long int)listener);
	}
	else
		 dt_print(DT_DEBUG_CAMCTL,"[camera_control] Registering already registered listener %lx\n",(unsigned long int)listener);
	pthread_mutex_unlock(&camctl->mutex);
}

void dt_camctl_unregister_listener( const dt_camctl_t *c, dt_camctl_listener_t *listener)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	pthread_mutex_lock(&camctl->mutex);
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Unregistering listener %lx\n",(unsigned long int)listener);
	camctl->listeners = g_list_remove( camctl->listeners, listener );
	pthread_mutex_unlock(&camctl->mutex);  
}

gint _compare_camera_by_port(gconstpointer a,gconstpointer b)
{
	dt_camera_t *ca=(dt_camera_t *)a;
	dt_camera_t *cb=(dt_camera_t *)b;
	return strcmp(ca->port,cb->port);
}

void dt_camctl_detect_cameras(const dt_camctl_t *c)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	CameraList *available_cameras=NULL;
	gp_list_new( &available_cameras );
	gp_abilities_list_detect (c->gpcams,c->gpports, available_cameras, c->gpcontext );
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] %d cameras connected\n",gp_list_count( available_cameras )>0?gp_list_count( available_cameras ):0);

	pthread_mutex_lock(&camctl->mutex);
	
	for(int i=0;i<gp_list_count( available_cameras );i++)
	{
		dt_camera_t *camera=g_malloc(sizeof(dt_camera_t));
		memset( camera,0,sizeof(dt_camera_t));
		gp_list_get_name (available_cameras, i, &camera->model);
		gp_list_get_value (available_cameras, i, &camera->port);
		
	 // if(strcmp(camera->port,"usb:")==0) { g_free(camera); continue; }
		
		GList *citem;
		if( (citem=g_list_find_custom(c->cameras,camera,_compare_camera_by_port)) == NULL || strcmp(((dt_camera_t *)citem->data)->model,camera->model)!=0 ) 
		{
			if(citem==NULL)
			{ // New camera
				
				pthread_mutex_init(&camera->lock, NULL);
				if(_camera_initialize(c,camera)==FALSE)
				{
					dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to initialize device %s on port %s\n", camera->model,camera->port);
					g_free(camera);
					continue;
				}
				
				if( camera->can_import==FALSE && camera->can_tether==FALSE )
				{
					dt_print(DT_DEBUG_CAMCTL,"[camera_control] Device %s on port %s can neither import or tether, skipping.\n", camera->model,camera->port);
					g_free(camera);
					continue;
				}
				
				if( gp_camera_get_summary(camera->gpcam, &camera->summary, c->gpcontext) == GP_OK )
				{
					// Remove device property summary:
					char *eos=strstr(camera->summary.text,"Device Property Summary:\n");
					eos[0]='\0';
				}
				camctl->cameras = g_list_append(camctl->cameras,camera);
			} 
			else
			{ // Update camera on port
				((dt_camera_t*)citem->data)->port=camera->port;
				g_free(camera);
			}
			
			// Notify listerners of conencted camera
			GList *listener;
			if((listener=g_list_first(camctl->listeners))!=NULL)
				do
				{
					if(((dt_camctl_listener_t*)listener->data)->camera_connected)
						((dt_camctl_listener_t*)listener->data)->camera_connected(camera,((dt_camctl_listener_t*)listener->data)->data);
				} while((listener=g_list_next(listener))!=NULL);
				
		} else g_free(camera);
		
	}
	pthread_mutex_unlock(&camctl->mutex);
}

static void *_camera_event_thread(void *data) {
	dt_camctl_t *camctl=(dt_camctl_t *)data;

	const dt_camera_t *camera=camctl->active_camera;
	
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Starting camera event thread %lx of context %lx\n",(unsigned long int)camctl->thread,(unsigned long int)data);
	
	while(camera->is_tethering==TRUE) 
	{
		_camera_poll_events(camctl,camera);
	}
	
	dt_print(DT_DEBUG_CAMCTL,"[camera_control] Exiting camera thread %lx.\n",(unsigned long int)camctl->thread);

	return NULL;
}

gboolean _camera_initialize(const dt_camctl_t *c, dt_camera_t *cam) {
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	CameraAbilities a;
	GPPortInfo pi;
	if( cam->gpcam==NULL )
	{
		gp_camera_new(&cam->gpcam);
		int m = gp_abilities_list_lookup_model( c->gpcams, cam->model );
		gp_abilities_list_get_abilities (c->gpcams, m, &a);
		gp_camera_set_abilities (cam->gpcam, a);
	
		int p = gp_port_info_list_lookup_path (c->gpports, cam->port);
		gp_port_info_list_get_info (c->gpports, p, &pi);
		gp_camera_set_port_info (cam->gpcam , pi);
		
		// Check for abilities
		if( (a.operations&GP_OPERATION_CAPTURE_IMAGE) ) cam->can_tether=TRUE;
		if(  cam->can_tether && (a.operations&GP_OPERATION_CONFIG) ) cam->can_config=TRUE;
		if( !(a.file_operations&GP_FILE_OPERATION_NONE) ) cam->can_import=TRUE;
		
		if( gp_camera_init( cam->gpcam ,  camctl->gpcontext) != GP_OK )
		{
			dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to initialize camera %s on port %s\n", cam->model,cam->port);
			return FALSE;
		}
			
		dt_print(DT_DEBUG_CAMCTL,"[camera_control] Device %s on port %s initialized\n", cam->model,cam->port);
	} else
		dt_print(DT_DEBUG_CAMCTL,"[camera_control] Device %s on port %s already initialized\n", cam->model,cam->port);
			
	return TRUE;
}

		

void dt_camctl_import(const dt_camctl_t *c,const dt_camera_t *cam,GList *images) {
	//dt_camctl_t *camctl=(dt_camctl_t *)c;
	_camctl_lock(c,cam);
	
	GList *ifile=g_list_first(images);
	
	const char *output_path=_dispatch_request_image_path(c,cam);
	if(ifile)
		do
		{
			// Split file into folder and filename
			char *eos;
			char folder[4096]={0};
			char filename[4096]={0};
			char *file=(char *)ifile->data;
			eos=file+strlen(file);
			while( --eos>file && *eos!='/' );
			strncat(folder,file,eos-file);
			strcat(filename,eos+1);
			
			const char *fname = _dispatch_request_image_filename(c,filename,cam);
			if(!fname) fname=filename;
			
			/*char outputfile[4096]={0};
			strcat(outputfile,output_path);
			if(outputfile[strlen(outputfile)]!='/') strcat(outputfile,"/");
			strcat(outputfile,filename);*/
			
			char *output = g_build_filename(output_path,fname,NULL);
			
			// Now we have filenames lets download file and notify listener of image download
			CameraFile *destination;
			int handle = open( output, O_CREAT | O_WRONLY,0666);
			if( handle > 0 ) {
				gp_file_new_from_fd( &destination , handle );
				if( gp_camera_file_get( cam->gpcam, folder , filename, GP_FILE_TYPE_NORMAL, destination,  c->gpcontext) == GP_OK)
				{
					close( handle );
					_dispatch_camera_image_downloaded(c,cam,output);
				}
			else
					dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to download file %s\n",output);
				
				
			}
		} while( (ifile=g_list_next(ifile)) );
	
		_dispatch_control_status(c,CAMERA_CONTROL_AVAILABLE);
	_camctl_unlock(c);
}

void _camctl_recursive_get_previews(const dt_camctl_t *c,char *path)
{
	CameraList *files;
	CameraList *folders;
	const char *filename;
	const char *foldername;
	
	gp_list_new (&files);
	gp_list_new (&folders);
	
	// Process files in current folder...
	if( gp_camera_folder_list_files(c->active_camera->gpcam,path,files,c->gpcontext) == GP_OK ) 
	{
		for(int i=0; i < gp_list_count(files); i++) 
		{
			char file[4096]={0};
			strcat(file,path);
			strcat(file,"/");
			gp_list_get_name (files, i, &filename);
			strcat(file,filename);
			
			// Lets check the type of file...
			CameraFileInfo cfi;
			if( gp_camera_file_get_info(c->active_camera->gpcam, path, filename,&cfi,c->gpcontext) == GP_OK)
			{
				// Let's slurp the preview image
				CameraFile *cf=NULL;
				gp_file_new(&cf);
				if( gp_camera_file_get(c->active_camera->gpcam, path, filename, GP_FILE_TYPE_PREVIEW,cf,c->gpcontext) < GP_OK )
				{
					// No preview for file lets check image size to se if we should download full image for preview...
					if( cfi.file.size > 0  && cfi.file.size < 512000 ) 
						if( gp_camera_file_get(c->active_camera->gpcam, path, filename, GP_FILE_TYPE_NORMAL,cf,c->gpcontext) < GP_OK )
						{
							cf=NULL;
							dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to retreive preview of file %s\n",filename);
						}
				}
					_dispatch_camera_storage_image_filename(c,c->active_camera,file,cf);
			}
			else
					dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to get file information of %s in folder %s on device\n",filename,path);
		}
	} 
	
	// Recurse into folders in current folder...
	if( gp_camera_folder_list_folders(c->active_camera->gpcam,path,folders,c->gpcontext)==GP_OK) 
	{
		for(int i=0; i < gp_list_count(folders); i++) 
		{
			char buffer[4096]={0};
			strcat(buffer,path);
			if(path[1]!='\0') strcat(buffer,"/");
			gp_list_get_name (folders, i, &foldername);
			strcat(buffer,foldername);
			_camctl_recursive_get_previews(c,buffer);
		}
	}
	 gp_list_free (files);
	 gp_list_free (folders);
}

void dt_camctl_select_camera(const dt_camctl_t *c, const dt_camera_t *cam)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	camctl->wanted_camera=cam;
}


void dt_camctl_get_previews(const dt_camctl_t *c,const dt_camera_t *cam) {
	_camctl_lock(c,cam);
	_camctl_recursive_get_previews(c,"/");
	_camctl_unlock(c);
}

void dt_camctl_tether_mode(const dt_camctl_t *c, const dt_camera_t *cam,gboolean enable)
{
	if( cam == NULL )
		cam=c->wanted_camera;
	
	if( cam && cam->can_tether ) 
	{
		
		
		dt_camctl_t *camctl=(dt_camctl_t *)c;
		dt_camera_t *camera=(dt_camera_t *)cam;
		
		if( enable==TRUE)
		{
			_camctl_lock(c,cam);
			// Start up camera event polling thread
			dt_print(DT_DEBUG_CAMCTL,"[camera_control] Enabling tether mode\n");
			camctl->active_camera=camera;
			camera->is_tethering=TRUE;
			pthread_create(&camctl->thread, NULL, &_camera_event_thread, (void *)c);
		} 
		else
		{
			camera->is_tethering=FALSE;
			dt_print(DT_DEBUG_CAMCTL,"[camera_control] Disabling tether mode\n");
			_camctl_unlock(c);
			// Wait for tether thread with join??
		}
	} else
		dt_print(DT_DEBUG_CAMCTL,"[camera_control] Failed to set tether mode with reason: %s\n", cam?"device does not support tethered capture":"no active camera");

}

void _camera_poll_events(const dt_camctl_t *c,const dt_camera_t *cam)
{
//  dt_camctl_t *camctl=(dt_camctl_t *)c;
	CameraEventType event;
	gpointer data;
	int res;
	gboolean wait_timedout=FALSE;
	while( !wait_timedout )
	{
		if( (res=gp_camera_wait_for_event( cam->gpcam, 100, &event, &data, c->gpcontext ) )>= GP_OK ) {
			switch( event ) {
				case GP_EVENT_UNKNOWN: 
				{
					if( strstr( (char *)data, "4006" ) ); // Property change event occured on camera
					
				} break;

				case GP_EVENT_FILE_ADDED:
				{
					if( cam->is_tethering ) 
					{
						dt_print(DT_DEBUG_CAMCTL,"[camera_control] Camera file added event\n");
						CameraFilePath *fp = (CameraFilePath *)data;
						CameraFile *destination;
						char filename[512]={0};
						const char *path=_dispatch_request_image_path(c,cam);
						if( path )
							strcat(filename,path);
						else
							strcat(filename,"/tmp/");
						strcat(filename,fp->name);
						int handle = open( filename, O_CREAT | O_WRONLY,0666);
						gp_file_new_from_fd( &destination , handle );
						gp_camera_file_get( cam->gpcam, fp->folder , fp->name, GP_FILE_TYPE_NORMAL, destination,  c->gpcontext);
						close( handle );
							
						// Notify listerners of captured image
						_dispatch_camera_image_downloaded(c,cam,filename);
						
					}
				} break;

				case GP_EVENT_TIMEOUT:
					wait_timedout=TRUE;
				break;
				
				case GP_EVENT_FOLDER_ADDED:
				case GP_EVENT_CAPTURE_COMPLETE:
				break;
				
			}
		} 
		else
		{
			// Catch any error and handle the situation..
		}
	}
}

const char *_dispatch_request_image_filename(const dt_camctl_t *c,const char *filename,const dt_camera_t *camera)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	const char *path=NULL;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->request_image_filename != NULL )
			path=((dt_camctl_listener_t*)listener->data)->request_image_filename(camera,filename,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
	return path;
}


const char *_dispatch_request_image_path(const dt_camctl_t *c,const dt_camera_t *camera)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	const char *path=NULL;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->request_image_path != NULL )
			path=((dt_camctl_listener_t*)listener->data)->request_image_path(camera,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
	return path;
}

void _dispatch_camera_connected(const dt_camctl_t *c,const dt_camera_t *camera)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->camera_connected != NULL )
			((dt_camctl_listener_t*)listener->data)->camera_connected(camera,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
}

void _dispatch_camera_disconnected(const dt_camctl_t *c,const dt_camera_t *camera)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->camera_disconnected != NULL )
			((dt_camctl_listener_t*)listener->data)->camera_disconnected(camera,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
}


void _dispatch_camera_image_downloaded(const dt_camctl_t *c,const dt_camera_t *camera,const char *filename)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->image_downloaded != NULL )
			((dt_camctl_listener_t*)listener->data)->image_downloaded(camera,filename,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
}

void _dispatch_camera_storage_image_filename(const dt_camctl_t *c,const dt_camera_t *camera,const char *filename,CameraFile *preview)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->camera_storage_image_filename != NULL )
			((dt_camctl_listener_t*)listener->data)->camera_storage_image_filename(camera,filename,preview,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
}

void _dispatch_control_status(const dt_camctl_t *c,dt_camctl_status_t status)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->control_status != NULL )
			((dt_camctl_listener_t*)listener->data)->control_status(status,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
}

void _dispatch_camera_error(const dt_camctl_t *c,const dt_camera_t *camera,dt_camera_error_t error)
{
	dt_camctl_t *camctl=(dt_camctl_t *)c;
	GList *listener;
	if((listener=g_list_first(camctl->listeners))!=NULL)
	do
	{
		if( ((dt_camctl_listener_t*)listener->data)->camera_error != NULL )
			((dt_camctl_listener_t*)listener->data)->camera_error(camera,error,((dt_camctl_listener_t*)listener->data)->data);
	} while((listener=g_list_next(listener))!=NULL);
}
