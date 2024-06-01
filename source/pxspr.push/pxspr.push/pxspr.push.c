#include <libusb.h>
#include <jit.common.h>

#define ABLETON_VENDOR_ID 0x2982
#define PUSH2_PRODUCT_ID 0x1967
#define PUSH2_BULK_EP_OUT 0x01
#define PUSH2_TRANSFER_TIMEOUT 1000

#define PUSH2_DISPLAY_WIDTH 960
#define PUSH2_DISPLAY_HEIGHT 160
#define PUSH2_DISPLAY_LINE_BUFFER_SIZE 2048
#define PUSH2_DISPLAY_LINE_GUTTER_SIZE 128
#define PUSH2_DISPLAY_LINE_DATA_SIZE PUSH2_DISPLAY_LINE_BUFFER_SIZE - 
#define PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE 16384
#define PUSH2_DISPLAY_IMAGE_BUFFER_SIZE PUSH2_DISPLAY_LINE_BUFFER_SIZE * PUSH2_DISPLAY_HEIGHT
#define PUSH2_DISPLAY_MESSAGES_PER_IMAGE (PUSH2_DISPLAY_LINE_BUFFER_SIZE * PUSH2_DISPLAY_HEIGHT) / PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE

#define PUSH2_DISPLAY_SHAPING_PATTERN_1 0xE7
#define PUSH2_DISPLAY_SHAPING_PATTERN_2 0xF3
#define PUSH2_DISPLAY_SHAPING_PATTERN_3 0xE7
#define PUSH2_DISPLAY_SHAPING_PATTERN_4 0xFF

#define PUSH2_DISPLAY_FRAMERATE 60

const unsigned char PUSH2_DISPLAY_FRAME_HEADER[] =
{ 0xFF, 0xCC, 0xAA, 0x88,
0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00 };


typedef struct _pxspr_push
{
	t_object object;

	t_systhread thread;
	t_systhread_mutex mutex;
	t_bool is_thread_cancel;

	t_uint8* draw_buffer;
	t_uint8* send_buffer;

	libusb_device_handle* device;
	
	t_bool status;

} t_pxspr_push;


BEGIN_USING_C_LINKAGE
t_jit_err pxspr_push_init();
t_pxspr_push* pxspr_push_new();
void pxspr_push_free(t_pxspr_push* x);
t_jit_err pxspr_push_matrix_calc(t_pxspr_push* x, void* inputs, void* outputs);
void pxspr_push_copyandmask_buffer(t_pxspr_push* x);
void* pxspr_push_threadproc(t_pxspr_push* x);
void pxspr_push_open_device(t_pxspr_push* x);
void pxspr_push_close_device(t_pxspr_push* x);
END_USING_C_LINKAGE


static t_class* s_pxspr_push_class = NULL;

static t_symbol* _sym_status;


t_jit_err pxspr_push_init()
{
	t_jit_object* mop;

	_sym_status = gensym("status");

	s_pxspr_push_class = (t_class*)jit_class_new("pxspr_push", (method)pxspr_push_new, (method)pxspr_push_free, sizeof(t_pxspr_push), 0);

	mop = (t_jit_object *)jit_object_new(_jit_sym_jit_mop, 1, 0);
	jit_mop_single_type(mop, _jit_sym_char);
	jit_mop_single_planecount(mop, 4);
	
	t_atom args[2];
	jit_atom_setlong(args, PUSH2_DISPLAY_WIDTH);
	jit_atom_setlong(args + 1, PUSH2_DISPLAY_HEIGHT);

	void* input = jit_object_method(mop, _jit_sym_getinput, 1);
	jit_object_method(input, _jit_sym_mindim, 2, &args);
	jit_object_method(input, _jit_sym_maxdim, 2, &args);
	jit_object_method(input, _jit_sym_ioproc, jit_mop_ioproc_copy_adapt);
	
	jit_class_addadornment(s_pxspr_push_class, mop);

	t_jit_object* attr = jit_object_new(_jit_sym_jit_attr_offset, "status", _jit_sym_char, 
		JIT_ATTR_GET_DEFER_LOW | JIT_ATTR_SET_USURP_LOW | JIT_ATTR_SET_OPAQUE_USER,
		(method)0L, (method)0L, calcoffset(t_pxspr_push, status));
	jit_class_addattr(s_pxspr_push_class, attr);

	jit_class_addmethod(s_pxspr_push_class, (method)pxspr_push_matrix_calc, "matrix_calc", A_CANT, 0);
	jit_class_addmethod(s_pxspr_push_class, (method)pxspr_push_open_device, "open", 0);
	jit_class_addmethod(s_pxspr_push_class, (method)pxspr_push_close_device, "close", 0);
	jit_class_addmethod(s_pxspr_push_class, (method)jit_object_register, "register", A_CANT, 0);

	jit_class_register(s_pxspr_push_class);
	return JIT_ERR_NONE;
}


t_pxspr_push* pxspr_push_new()
{
	t_pxspr_push* x = NULL;

	x = (t_pxspr_push*)jit_object_alloc(s_pxspr_push_class);
	if (x)
	{
		x->status = FALSE;

		systhread_mutex_new(&x->mutex, 0);

		x->draw_buffer = (t_uint8*)sysmem_newptrclear(PUSH2_DISPLAY_IMAGE_BUFFER_SIZE);
		x->send_buffer = (t_uint8*)sysmem_newptrclear(PUSH2_DISPLAY_IMAGE_BUFFER_SIZE);
		pxspr_push_copyandmask_buffer(x);

		defer_low(x, (method)pxspr_push_open_device, NULL, 0, NULL);
	}
	return x;
}

void pxspr_push_free(t_pxspr_push* x)
{
	pxspr_push_close_device(x);

	systhread_mutex_free(x->mutex);

	sysmem_freeptr(x->draw_buffer);
	sysmem_freeptr(x->send_buffer);
}

t_jit_err pxspr_push_matrix_calc(t_pxspr_push* x, void* inputs, void* outputs)
{
	void* input_matrix = jit_object_method(inputs, _jit_sym_getindex, 0);

	if (x == NULL || input_matrix == NULL)
		return JIT_ERR_INVALID_PTR;

	long lock = (long)jit_object_method(input_matrix, _jit_sym_lock, 1);

	t_jit_matrix_info	input_info;
	jit_object_method(input_matrix, _jit_sym_getinfo, &input_info);

	if (input_info.dimcount != 2)
	{
		object_error((t_object*)x, "Input matrix must have 2 dimensions");
		jit_object_method(input_matrix, _jit_sym_lock, lock);
		return JIT_ERR_INVALID_INPUT;
	}

	if (input_info.planecount != 3 && input_info.planecount != 4)
	{
		object_error((t_object*)x, "Input matrix must have 3 planes (RGB) or 4 planes (ARGB)");
		jit_object_method(input_matrix, _jit_sym_lock, lock);
		return JIT_ERR_INVALID_INPUT;
	}

	if (input_info.dim[0] != PUSH2_DISPLAY_WIDTH || input_info.dim[1] != PUSH2_DISPLAY_HEIGHT)
	{
		object_error((t_object*)x, "Input matrix must have dimensions of 960 x 160");
		jit_object_method(input_matrix, _jit_sym_lock, lock);
		return JIT_ERR_INVALID_INPUT;
	}

	char* input_data;
	jit_object_method(input_matrix, _jit_sym_getdata, &input_data);

	if (input_data == NULL)
	{
		jit_object_method(input_matrix, _jit_sym_lock, lock);
		return JIT_ERR_INVALID_INPUT;
	}
	
	t_uint8* src = (t_uint8*)input_data;

	t_uint16* dst = (t_uint16*)x->draw_buffer;

	if (input_info.planecount == 3)
	{
		for (int dX = 0; dX < PUSH2_DISPLAY_HEIGHT; ++dX)
		{
			for (int dY = 0; dY < PUSH2_DISPLAY_WIDTH; ++dY)
			{
				*dst++ = (*(src) >> 3) | ((*(src + 1) & 0xFC) << 3) | ((*(src + 2) & 0xF8) << 8);
				src += 3;
			}

			dst += PUSH2_DISPLAY_LINE_GUTTER_SIZE / 2;
		}
	}
	else
	{
		for (int dX = 0; dX < PUSH2_DISPLAY_HEIGHT; ++dX)
		{
			for (int dY = 0; dY < PUSH2_DISPLAY_WIDTH; ++dY)
			{
				*dst++ = (*(src + 1) >> 3) | ((*(src + 2) & 0xFC) << 3) | ((*(src + 3) & 0xF8) << 8);
				src += 4;
			}

			dst += PUSH2_DISPLAY_LINE_GUTTER_SIZE / 2;
		}
	}

	pxspr_push_copyandmask_buffer(x);

	return JIT_ERR_NONE;
}

void pxspr_push_copyandmask_buffer(t_pxspr_push* x)
{
	systhread_mutex_lock(x->mutex);

	uint8_t* src = x->draw_buffer;
	uint8_t* dst = x->send_buffer;

	for(int dY = 0; dY < PUSH2_DISPLAY_HEIGHT; ++dY)
	{
		for(int dX = 0; dX < PUSH2_DISPLAY_LINE_BUFFER_SIZE - PUSH2_DISPLAY_LINE_GUTTER_SIZE; dX += 4)
		{
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_1;
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_2;
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_3;
			*(dst++) = *(src++) ^ PUSH2_DISPLAY_SHAPING_PATTERN_4;
		}

		src += PUSH2_DISPLAY_LINE_GUTTER_SIZE;
		dst += PUSH2_DISPLAY_LINE_GUTTER_SIZE;
	}

	systhread_mutex_unlock(x->mutex);
}

void* pxspr_push_threadproc(t_pxspr_push* x)
{
	int result;

	while(!x->is_thread_cancel)
	{
		if (x->device != NULL)
		{
			int actual_length;

			result = libusb_bulk_transfer(
				x->device,
				PUSH2_BULK_EP_OUT,
				(unsigned char*)PUSH2_DISPLAY_FRAME_HEADER,
				sizeof(PUSH2_DISPLAY_FRAME_HEADER),
				&actual_length,
				PUSH2_TRANSFER_TIMEOUT);

			if (result != 0)
				break;

            
            systhread_mutex_lock(x->mutex);
            
			for (int i = 0; i < PUSH2_DISPLAY_MESSAGES_PER_IMAGE; ++i)
			{
				result = libusb_bulk_transfer(
					x->device,
					PUSH2_BULK_EP_OUT,
					x->send_buffer + (i * PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE),
					PUSH2_DISPLAY_MESSAGE_BUFFER_SIZE,
					&actual_length,
					PUSH2_TRANSFER_TIMEOUT);

				if (result != 0)
					break;
			}

			systhread_mutex_unlock(x->mutex);

			if (result != 0)
				break;
		}

		systhread_sleep(1000 / PUSH2_DISPLAY_FRAMERATE);
	}

	libusb_release_interface(x->device, 0);
	libusb_close(x->device);
	x->device = NULL;

	jit_attr_setlong(x, _sym_status, 0);
	
	return 0;
}

void pxspr_push_open_device(t_pxspr_push* x)
{
	if (x->device != NULL)
		return;

	int result;

	if ((result = libusb_init(NULL)) < 0)
	{
		object_error((t_object*)x, "Failed to initilialize libusb", result);
		return;
	}

	libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_ERROR);

	libusb_device** devices;
	ssize_t count;
	count = libusb_get_device_list(NULL, &devices);
	if (count < 0)
	{
		object_error((t_object*)x, "Failed to get USB device list");
		return;
	}

	libusb_device* device;
	libusb_device_handle* device_handle = NULL;

	for (int i = 0; (device = devices[i]) != NULL; ++i)
	{
		struct libusb_device_descriptor descriptor;
		if ((result = libusb_get_device_descriptor(device, &descriptor)) < 0)
		{
			object_error((t_object*)x, "Failed to get USB device descriptor");
			continue;
		}

		if (descriptor.bDeviceClass == LIBUSB_CLASS_PER_INTERFACE
			&& descriptor.idVendor == ABLETON_VENDOR_ID
			&& descriptor.idProduct == PUSH2_PRODUCT_ID)
		{
			if ((result = libusb_open(device, &device_handle)) < 0)
			{
				object_error((t_object*)x, "Failed to open Push 2 device");
			}
			else if ((result = libusb_claim_interface(device_handle, 0)) < 0)
			{
				object_error((t_object*)x, "Failed to open Push 2 device, may be in use by another application");
				libusb_close(device_handle);
				device_handle = NULL;
			}
			else
			{
				// Successfully opened
				break; 
			}
		}
	}

	libusb_free_device_list(devices, 1);
	x->device = device_handle;

	if (x->device != NULL)
	{
		systhread_create((method)pxspr_push_threadproc, x, 0, 0, 0, &x->thread);
		jit_attr_setlong(x, _sym_status, 1);
	}
}

void pxspr_push_close_device(t_pxspr_push* x)
{
	if (x->device == NULL)
		return;

	x->is_thread_cancel = TRUE;
	unsigned int value;
	systhread_join(x->thread, &value);
	x->is_thread_cancel = FALSE;
	x->thread = NULL;
}