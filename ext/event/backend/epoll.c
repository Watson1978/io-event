// Copyright, 2021, by Samuel G. D. Williams. <http://www.codeotaku.com>
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "kqueue.h"

#include <sys/epoll.h>
#include <time.h>
#include <errno.h>

static VALUE Event_Backend_EPoll = Qnil;
static ID id_fileno, id_transfer;

static const int READABLE = 1, PRIORITY = 2, WRITABLE = 4;

static const unsigned EPOLL_MAX_EVENTS = 1024;

struct Event_Backend_EPoll {
	VALUE loop;
	int descriptor;
};

void Event_Backend_EPoll_Type_mark(void *_data)
{
	struct Event_Backend_EPoll *data = _data;
	rb_gc_mark(data->loop);
}

void Event_Backend_EPoll_Type_free(void *_data)
{
	struct Event_Backend_EPoll *data = _data;
	
	if (data->descriptor >= 0) {
		close(data->descriptor);
	}
	
	free(data);
}

size_t Event_Backend_EPoll_Type_size(const void *data)
{
	return sizeof(struct Event_Backend_EPoll);
}

static const rb_data_type_t Event_Backend_EPoll_Type = {
	.wrap_struct_name = "Event::Backend::EPoll",
	.function = {
		.dmark = Event_Backend_EPoll_Type_mark,
		.dfree = Event_Backend_EPoll_Type_free,
		.dsize = Event_Backend_EPoll_Type_size,
	},
	.data = NULL,
	.flags = RUBY_TYPED_FREE_IMMEDIATELY,
};

VALUE Event_Backend_EPoll_allocate(VALUE self) {
	struct Event_Backend_EPoll *data = NULL;
	VALUE instance = TypedData_Make_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	data->loop = Qnil;
	data->descriptor = -1;
	
	return instance;
}

VALUE Event_Backend_EPoll_initialize(VALUE self, VALUE loop) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	data->loop = loop;
	data->descriptor = epoll_create(1);
	
	return self;
}

VALUE Event_Backend_EPoll_io_wait(VALUE self, VALUE fiber, VALUE io, VALUE events) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct epoll_event event = {0};
	
	int descriptor = NUM2INT(rb_funcall(io, id_fileno, 0));
	int duplicate = -1;
	
	int mask = NUM2INT(events);
	
	if (mask & READABLE) {
		event.events |= EPOLLIN;
	}
	
	if (mask & PRIORITY) {
		event.events |= EPOLLPRI;
	}
	
	if (mask & WRITABLE) {
		event.events |= EPOLLOUT;
	}
	
	event.events |= EPOLLRDHUP;
	event.events |= EPOLLONESHOT;
	
	event.data.ptr = (void*)fiber;
	
	// A better approach is to batch all changes:
	int result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);;
	
	if (result == -1 && errno == EEXIST) {
		// The file descriptor was already inserted into epoll.
		duplicate = descriptor = dup(descriptor);
		
		rb_update_max_fd(duplicate);
		
		if (descriptor == -1)
			rb_sys_fail("dup");
		
		result = epoll_ctl(data->descriptor, EPOLL_CTL_ADD, descriptor, &event);;
	}
	
	if (result == -1) {
		rb_sys_fail("epoll_ctl");
	}
	
	rb_funcall(data->loop, id_transfer, 0);
	
	if (duplicate >= 0) {
		close(duplicate);
	}
	
	return Qnil;
}

static
int make_timeout(VALUE duration) {
	if (duration == Qnil) {
		return -1;
	}
	
	if (FIXNUM_P(duration)) {
		return NUM2LONG(duration) * 1000L;
	}
	
	else if (RB_FLOAT_TYPE_P(duration)) {
		double value = RFLOAT_VALUE(duration);
		
		return value * 1000;
	}
	
	rb_raise(rb_eRuntimeError, "unable to convert timeout");
}

VALUE Event_Backend_EPoll_select(VALUE self, VALUE duration) {
	struct Event_Backend_EPoll *data = NULL;
	TypedData_Get_Struct(self, struct Event_Backend_EPoll, &Event_Backend_EPoll_Type, data);
	
	struct epoll_event events[EPOLL_MAX_EVENTS];
	
	int count = epoll_wait(data->descriptor, events, EPOLL_MAX_EVENTS, make_timeout(duration));
	
	if (count == -1) {
		rb_sys_fail("epoll_wait");
	}
	
	for (int i = 0; i < count; i += 1) {
		VALUE fiber = (VALUE)events[i].data.ptr;
		rb_funcall(fiber, id_transfer, 0);
	}
	
	return INT2NUM(count);
}

void Init_Event_Backend_EPoll(VALUE Event_Backend) {
	id_fileno = rb_intern("fileno");
	id_transfer = rb_intern("transfer");
	
	Event_Backend_EPoll = rb_define_class_under(Event_Backend, "EPoll", rb_cObject);
	
	rb_define_alloc_func(Event_Backend_EPoll, Event_Backend_EPoll_allocate);
	rb_define_method(Event_Backend_EPoll, "initialize", Event_Backend_EPoll_initialize, 1);
	
	rb_define_method(Event_Backend_EPoll, "io_wait", Event_Backend_EPoll_io_wait, 3);
	rb_define_method(Event_Backend_EPoll, "select", Event_Backend_EPoll_select, 1);
}
