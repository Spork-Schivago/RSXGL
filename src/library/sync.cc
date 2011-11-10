// RSXGL - Graphics library for the PS3 GPU.
//
// Copyright (c) 2011 Alexander Betts (alex.betts@gmail.com)
//
// sync.cc - Implement the glFlush and glFinish functions, and OpenGL synchronization objects.

#include <GL3/gl3.h>

#include "gl_fifo.h"
#include "rsxgl_assert.h"
#include "debug.h"
#include "error.h"
#include "rsxgl_context.h"
#include "state.h"
#include "program.h"
#include "attribs.h"
#include "uniforms.h"
#include "sync.h"

#include "gl_object.h"

#if defined(GLAPI)
#undef GLAPI
#endif
#define GLAPI extern "C"

static inline void
rsxgl_flush(rsxgl_context_t * ctx)
{
  rsxgl_gcm_flush(ctx -> gcm_context());
}

GLAPI void APIENTRY
glFlush (void)
{
  rsxgl_flush(current_ctx());

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glFinish (void)
{
  rsxgl_context_t * ctx = current_ctx();

  // TODO - Rumor has it that waiting on ctx -> ref is "slow". See if this is unacceptable, and see if a sync object is any better.
  const uint32_t ref = ctx -> ref++;
  rsxgl_emit_set_ref(ctx -> gcm_context(),ref);
  rsxgl_flush(ctx);

  gcmControlRegister volatile *control = gcmGetControlRegister();

  __sync();

  const useconds_t timeout = RSXGL_SYNC_SLEEP_INTERVAL * RSXGL_FINISH_SLEEP_ITERATIONS;
  const useconds_t timeout_interval = RSXGL_SYNC_SLEEP_INTERVAL;

  // Wait some interval for the GPU to finish:
  if(timeout > 0) {
    useconds_t remaining_timeout = timeout;
    while(control -> ref != ref && remaining_timeout > 0) {
      if(timeout_interval > 0) {
	usleep(timeout_interval);
	remaining_timeout -= timeout_interval;
      }
    }
  }
  // Wait forever:
  else {
    while(control -> ref != ref) {
      if(timeout_interval > 0) {
	usleep(timeout_interval);
      }
    }
  }

  RSXGL_NOERROR_();
}

// Sync objects are not considered true "GL objects," but they do require library-generated names.
// So we re-use that capability from gl_object<>. But since they can't be bound or orphaned, etc.,
// this class does not use the CRTP the way that other GL objects do.
struct rsxgl_sync_object_t {
  typedef gl_object< rsxgl_sync_object_t, RSXGL_MAX_SYNC_OBJECTS > gl_object_type;
  typedef typename gl_object_type::name_type name_type;
  typedef typename gl_object_type::storage_type storage_type;

  static storage_type & storage();

  uint32_t status:1, index:8, value:23;

  rsxgl_sync_object_t()
    : status(0), index(0), value(0) {
  }
};

rsxgl_sync_object_t::storage_type &
rsxgl_sync_object_t::storage()
{
  static rsxgl_sync_object_t::storage_type _storage(RSXGL_MAX_SYNC_OBJECTS);
  return _storage;
}

static inline rsxgl_sync_object_t::name_type
rsxgl_sync_object_really_allocate()
{
  const rsxgl_sync_object_t::name_type name = rsxgl_sync_object_t::storage().create_name();
  if(name > RSXGL_MAX_SYNC_OBJECTS) {
    rsxgl_sync_object_t::storage().destroy(name);
    return 0;
  }
  else {
    rsxgl_assert(name > 0);
    rsxgl_sync_object_t::storage().create_object(name);
    return name;
  }
}

static inline void
rsxgl_sync_object_really_free(const rsxgl_sync_object_t::name_type name)
{
  if(rsxgl_sync_object_t::storage().is_name(name)) {
    rsxgl_assert(rsxgl_sync_object_t::storage().is_object(name));
    rsxgl_sync_object_t::storage().destroy(name);
  }
}

static inline uint8_t
rsxgl_sync_name_to_rsx_index(const rsxgl_sync_object_t::name_type name)
{
  rsxgl_assert(name > 0 && name <= RSXGL_MAX_SYNC_OBJECTS);
  return (name - 1) + 64;
}

static inline rsxgl_sync_object_t::name_type
rsxgl_rsx_index_to_sync_name(const uint8_t index)
{
  rsxgl_assert(index >= 64 && index < (64 + RSXGL_MAX_SYNC_OBJECTS));
  return (index - 64) + 1;
}

uint8_t
rsxgl_sync_object_allocate()
{
  rsxgl_sync_object_t::name_type name = rsxgl_sync_object_really_allocate();
  return (name == 0) ? 0 : rsxgl_sync_name_to_rsx_index(name);
}

void
rsxgl_sync_object_free(const uint8_t index)
{
  if(index >= 64) {
    rsxgl_sync_object_really_free(rsxgl_rsx_index_to_sync_name(index));
  }
}

static const uint32_t rsxgl_sync_token_max = (1 << 23);
static const uint32_t RSXGL_SYNC_UNSIGNALED_TOKEN = (1 << 24);
static uint32_t _rsxgl_sync_token = 0;

static uint32_t
rsxgl_sync_token()
{
  return (_rsxgl_sync_token = (_rsxgl_sync_token + 1) & rsxgl_sync_token_max);
}

GLAPI GLsync APIENTRY
glFenceSync (GLenum condition, GLbitfield flags)
{
  if(condition != GL_SYNC_GPU_COMMANDS_COMPLETE) {
    RSXGL_ERROR(GL_INVALID_ENUM,0);
  }

  if(flags != 0) {
    RSXGL_ERROR(GL_INVALID_VALUE,0);
  }

  rsxgl_sync_object_t::name_type name = rsxgl_sync_object_really_allocate();

  if(name == 0) {
    RSXGL_ERROR(GL_OUT_OF_MEMORY,0);
  }
  else {
    rsxgl_assert(rsxgl_sync_object_t::storage().is_object(name));

    const uint8_t index = rsxgl_sync_name_to_rsx_index(name);
    const uint32_t token = rsxgl_sync_token();

    rsxgl_sync_object_t * sync_object = &rsxgl_sync_object_t::storage().at(name);
    sync_object -> status = 0;
    sync_object -> index = index;
    sync_object -> value = token;

    rsxgl_sync_cpu_signal(index,RSXGL_SYNC_UNSIGNALED_TOKEN);
    rsxgl_emit_sync_gpu_signal_read(current_ctx() -> base.gcm_context,sync_object -> index,token);

    RSXGL_NOERROR((GLsync)sync_object);
  }
}

GLAPI GLboolean APIENTRY
glIsSync (GLsync sync)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object != 0 && rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    rsxgl_assert(rsxgl_sync_object_t::storage().is_object(rsxgl_rsx_index_to_sync_name(sync_object -> index)));
    return GL_TRUE;
  }
  else {
    return GL_FALSE;
  }
}

GLAPI void APIENTRY
glDeleteSync (GLsync sync)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object == 0 || !rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  rsxgl_sync_object_t::name_type name = rsxgl_rsx_index_to_sync_name(sync_object -> index);
  rsxgl_assert(rsxgl_sync_object_t::storage().is_object(name));

  // TODO - if it's unsignaled, orphan it instead:
  rsxgl_sync_object_t::storage().destroy(name);
}

GLAPI GLenum APIENTRY
glClientWaitSync (GLsync sync, GLbitfield flags, GLuint64 timeout)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object == 0 || !rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    RSXGL_ERROR(GL_INVALID_VALUE,GL_WAIT_FAILED);
  }
  
  static const GLbitfield valid_flags = GL_SYNC_FLUSH_COMMANDS_BIT;
  if((flags & ~valid_flags) != 0) {
    RSXGL_ERROR(GL_INVALID_VALUE,GL_WAIT_FAILED);
  }

  rsxgl_assert(rsxgl_sync_object_t::storage().is_object(rsxgl_rsx_index_to_sync_name(sync_object -> index)));

  rsxgl_context_t * ctx = current_ctx();

  // Flush it all:
  if(flags & GL_SYNC_FLUSH_COMMANDS_BIT) {
    rsxgl_flush(ctx);
  }

  // Maybe it's already been set?
  if(sync_object -> status) {
    RSXGL_NOERROR(GL_ALREADY_SIGNALED);
  }

  // Timeout is nanoseconds - convert to microseconds:
  const useconds_t timeout_usec = timeout / 1000;

  const int result = rsxgl_sync_cpu_wait(sync_object -> index,sync_object -> value,timeout_usec,RSXGL_SYNC_SLEEP_INTERVAL);

  if(result) {
    sync_object -> status = 1;
    RSXGL_NOERROR(GL_CONDITION_SATISFIED);
  }
  else {
    RSXGL_NOERROR(GL_TIMEOUT_EXPIRED);
  }
}

GLAPI void APIENTRY
glWaitSync (GLsync sync, GLbitfield flags, GLuint64 timeout)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object == 0 || !rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  if(flags != 0 || timeout != GL_TIMEOUT_IGNORED) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  rsxgl_assert(rsxgl_sync_object_t::storage().is_object(rsxgl_rsx_index_to_sync_name(sync_object -> index)));

  // Doesn't seem like there's much to do here - this function seems useful mainly for an implementation that
  // supports switching between multiple GL contexts that are possibly running on different GPU's (which RSXGL
  // don't support), or for a future version of OpenGL that specifies a way for the CPU to signal the sync object.
  // This function is supposed to cause the GPU to block until a sync object is signalled - but the GL_ARB_sync
  // extension specifies that only the GPU itself can perform the signalling by calling glFenceSync only,
  // and the passing of a valid sync object to this function implies that would have been done already.
  // Still checked, dutifully, for GL errors, tho.

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
glGetSynciv (GLsync sync, GLenum pname, GLsizei bufSize, GLsizei *length, GLint *values)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object == 0 || !rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  rsxgl_sync_object_t::name_type name = rsxgl_rsx_index_to_sync_name(sync_object -> index);
  rsxgl_assert(rsxgl_sync_object_t::storage().is_object(name));

  if(bufSize < 1) {
    if(length != 0) *length = 0;
    RSXGL_NOERROR_();
  }

  if(pname == GL_OBJECT_TYPE) {
    *values = GL_SYNC_FENCE;
  }
  else if(pname == GL_SYNC_STATUS) {
    *values = (sync_object -> status) ? GL_SIGNALED : GL_UNSIGNALED;
  }
  else if(pname == GL_SYNC_CONDITION) {
    *values = GL_SYNC_GPU_COMMANDS_COMPLETE;
  }
  else if(pname == GL_SYNC_FLAGS) {
    *values = 0;
  }
  else {
    RSXGL_ERROR_(GL_INVALID_ENUM);
  }

  if(length != 0) *length = 1;
  RSXGL_NOERROR_();
}

#if 0
// Dumb synchronization extension. I was using these to test some things related to buffer streaming. Might want them again,
// but hopefully not. They implement the ability to tell the GPU to wait for a signal set by the CPU, something which
// is supported by the RSX but not presently called-for by the GL spec.
GLAPI GLsync APIENTRY
rsxglCreateSync(GLuint value)
{
  rsxgl_debug_printf("%s: %u\n",__PRETTY_FUNCTION__,value);
  
  rsxgl_sync_object_t::name_type name = rsxgl_sync_object_really_allocate();

  if(name == 0) {
    RSXGL_ERROR(GL_OUT_OF_MEMORY,0);
  }
  else {
    rsxgl_assert(rsxgl_sync_object_t::storage().is_object(name));

    const uint8_t index = rsxgl_sync_name_to_rsx_index(name);
    const uint32_t token = rsxgl_sync_token();

    rsxgl_sync_object_t * sync_object = &rsxgl_sync_object_t::storage().at(name);
    sync_object -> status = 0;
    sync_object -> index = index;
    sync_object -> value = value;

    rsxgl_sync_cpu_signal(index,value);

    RSXGL_NOERROR((GLsync)sync_object);
  }
}

GLAPI void APIENTRY
rsxglSyncGPUWait(GLsync sync,GLuint value)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object == 0 || !rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  rsxgl_debug_printf("%s: %u %u\n",__PRETTY_FUNCTION__,sync_object -> index,value);
  rsxgl_sync_gpu_wait(current_ctx() -> gcm_context(),sync_object -> index,value);

  RSXGL_NOERROR_();
}

GLAPI void APIENTRY
rsxglSyncCPUSignal(GLsync sync,GLuint value)
{
  rsxgl_sync_object_t * sync_object = (rsxgl_sync_object_t *)sync;
  if(sync_object == 0 || !rsxgl_sync_object_t::storage().is_name(rsxgl_rsx_index_to_sync_name(sync_object -> index))) {
    RSXGL_ERROR_(GL_INVALID_VALUE);
  }

  rsxgl_debug_printf("%s: %u %u\n",__PRETTY_FUNCTION__,sync_object -> index,value);
  rsxgl_sync_cpu_signal(sync_object -> index,value);

  RSXGL_NOERROR_();
}
#endif
