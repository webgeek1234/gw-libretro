#include <libretro.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include <gwlua.h>

/*---------------------------------------------------------------------------*/

static void dummy_log( enum retro_log_level level, const char* fmt, ... )
{
  (void)level;
  (void)fmt;
}

#define SRAM_MAX 8

typedef struct
{
  char types[ SRAM_MAX ];
  char keys[ SRAM_MAX ][ 32 ];
  char values[ SRAM_MAX ][ 64 ];
  char count;
}
sram_t;

retro_log_printf_t log_cb = dummy_log;
retro_environment_t env_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static struct retro_perf_callback perf_cb;

static int     init;
static gwrom_t rom;
static gwlua_t state;
static sram_t  sram;
static int     offset;
static int     soft_width;
static int     soft_height;

static struct retro_input_descriptor input_descriptors[] =
{
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
  { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
  // TODO: Is this needed?
  { 255, 255, 255, 255, NULL }
};

#ifdef LOG_PERFORMANCE
#define RETRO_PERFORMANCE_INIT(name)  static struct retro_perf_counter name = {#name}; if (!name.registered) perf_cb.perf_register(&(name))
#define RETRO_PERFORMANCE_START(name) perf_cb.perf_start(&(name))
#define RETRO_PERFORMANCE_STOP(name)  perf_cb.perf_stop(&(name))
#else
#define RETRO_PERFORMANCE_INIT(name)
#define RETRO_PERFORMANCE_START(name)
#define RETRO_PERFORMANCE_STOP(name)
#endif

/*---------------------------------------------------------------------------*/
/* gwlua user-defined functions */

const char* gwlua_load_value( gwlua_t* state, const char* key, int* type )
{
  for ( int i = 0; i < sram.count; i++ )
  {
    if ( !strcmp( sram.keys[ i ], key ) )
    {
      *type = sram.types[ i ];
      return sram.values[ i ];
    }
  }
  
  return NULL;
}

void gwlua_save_value( gwlua_t* state, const char* key, const char* value, int type )
{
  if ( sram.count < SRAM_MAX )
  {
    int ndx = sram.count++;
    
    sram.types[ ndx ] = type;
    
    strncpy( sram.keys[ ndx ], key, sizeof( sram.keys[ ndx ] ) );
    sram.keys[ ndx ][ sizeof( sram.keys[ ndx ] ) - 1 ] = 0;
    
    strncpy( sram.values[ ndx ], value, sizeof( sram.values[ ndx ] ) );
    sram.values[ ndx ][ sizeof( sram.values[ ndx ] ) - 1 ] = 0;
  }
  
  /* TODO: return an error when SRAM is full */
}

int gwlua_set_fb( unsigned width, unsigned height )
{
  struct retro_game_geometry geometry;
  
  geometry.base_width = width;
  geometry.base_height = height;
  geometry.max_width = width;
  geometry.max_height = height;
  geometry.aspect_ratio = 0.0f;
  
  env_cb( RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry );
  
  offset = 0;
  soft_width = width;
  soft_height = height;
  
  return 0;
}

void gwlua_zoom( gwlua_t* state, int x0, int y0, int width, int height )
{
  struct retro_game_geometry geometry;
  
  if ( x0 >= 0 )
  {
    geometry.base_width = width;
    geometry.base_height = height;
    soft_width = width;
    soft_height = height;
    offset = y0 * state->width + x0;
  }
  else
  {
    geometry.base_width = state->width;
    geometry.base_height = state->height;
    soft_width = state->width;
    soft_height = state->height;
    offset = 0;
  }
  
  geometry.max_width = state->width;
  geometry.max_height = state->height;
  geometry.aspect_ratio = 0.0f;
  
  env_cb( RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry );
}

void gwlua_vlog( const char* format, va_list args )
{
  char buffer[ 8192 ]; /* should be enough */
  
  vsnprintf( buffer, sizeof( buffer ), format, args );
  buffer[ sizeof( buffer ) - 1 ] = 0;
  log_cb( RETRO_LOG_ERROR, "%s", buffer );
}

/*---------------------------------------------------------------------------*/

void retro_get_system_info( struct retro_system_info* info )
{
  info->library_name = "Game & Watch";
  info->library_version = "1.0";
  info->need_fullpath = false;
  info->block_extract = false;
  info->valid_extensions = "mgw";
}

void retro_set_environment( retro_environment_t cb )
{
  env_cb = cb;
  
  static const struct retro_variable vars[] = {
    { NULL, NULL },
  };
  
  static const struct retro_controller_description controllers[] = {
    { "Controller", RETRO_DEVICE_JOYPAD },
    // TODO: Is this needed?
    { NULL, 0 }
  };
  
  static const struct retro_controller_info ports[] = {
    { controllers, 1 },
    { NULL, 0 }
  };
  
  cb( RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars );
  cb( RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports );
}

unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

void retro_init()
{
  struct retro_log_callback log;
  
  if ( env_cb( RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log ) )
  {
    log_cb = log.log;
  }
  
  // Always get the perf interface because we need it for the timers
  if ( !env_cb( RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb ) )
  {
    perf_cb.get_time_usec = NULL;
    log_cb( RETRO_LOG_WARN, "Could not get the perf interface\n" );
  }
}

void* constcast( const void* ptr );
extern const char* gw_gitstamp;
extern const char* rl_gitstamp;

bool retro_load_game( const struct retro_game_info* info )
{
  log_cb( RETRO_LOG_ERROR, "\n%s\n%s", gw_gitstamp, rl_gitstamp );
  
  if ( !perf_cb.get_time_usec )
  {
    log_cb( RETRO_LOG_ERROR, "Core needs the perf interface\n" );
    return false;
  }
  
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
  
  if ( !env_cb( RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt ) )
  {
    log_cb( RETRO_LOG_ERROR, "RGB565 is not supported\n" );
    return false;
  }
  
  int res = gwrom_init( &rom, constcast( info->data ), info->size, GWROM_COPY_ALWAYS );
  
  if ( res != GWROM_OK )
  {
    log_cb( RETRO_LOG_ERROR, "Error initializing the rom: ", gwrom_error_message( res ) );
    init = -1;
    return false;
  }
  
  env_cb( RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors );
  memset( (void*)&state, 0, sizeof( state ) );
  state.width = state.height = 128;
  init = 0;
  return true;
}

size_t retro_get_memory_size( unsigned id )
{
  return id == RETRO_MEMORY_SAVE_RAM ? sizeof( sram ) : 0;
}

void* retro_get_memory_data( unsigned id )
{
  return id == RETRO_MEMORY_SAVE_RAM ? (void*)&sram : NULL;
}

void retro_set_video_refresh( retro_video_refresh_t cb )
{
  video_cb = cb;
}

void retro_set_audio_sample( retro_audio_sample_t cb )
{
  (void)cb;
}

void retro_set_audio_sample_batch( retro_audio_sample_batch_t cb )
{
  audio_cb = cb;
}

void retro_set_input_state( retro_input_state_t cb )
{
  input_state_cb = cb;
}

void retro_set_input_poll( retro_input_poll_t cb )
{
  input_poll_cb = cb;
}

void retro_get_system_av_info( struct retro_system_av_info* info )
{
  info->geometry.base_width = state.width;
  info->geometry.base_height = state.height;
  info->geometry.max_width = state.width;
  info->geometry.max_height = state.height;
  info->geometry.aspect_ratio = 0.0f;
  info->timing.fps = 60.0;
  info->timing.sample_rate = 44100.0;
}

void retro_run()
{
  static const struct { unsigned retro; int gw; } map[] =
  {
    { RETRO_DEVICE_ID_JOYPAD_UP,     GWLUA_UP },
    { RETRO_DEVICE_ID_JOYPAD_DOWN,   GWLUA_DOWN },
    { RETRO_DEVICE_ID_JOYPAD_LEFT,   GWLUA_LEFT },
    { RETRO_DEVICE_ID_JOYPAD_RIGHT,  GWLUA_RIGHT },
    { RETRO_DEVICE_ID_JOYPAD_A,      GWLUA_A },
    { RETRO_DEVICE_ID_JOYPAD_B,      GWLUA_B },
    { RETRO_DEVICE_ID_JOYPAD_X,      GWLUA_X },
    { RETRO_DEVICE_ID_JOYPAD_Y,      GWLUA_Y },
    { RETRO_DEVICE_ID_JOYPAD_L,      GWLUA_L1 },
    { RETRO_DEVICE_ID_JOYPAD_R,      GWLUA_R1 },
    { RETRO_DEVICE_ID_JOYPAD_L2,     GWLUA_L2 },
    { RETRO_DEVICE_ID_JOYPAD_R2,     GWLUA_R2 },
    { RETRO_DEVICE_ID_JOYPAD_L3,     GWLUA_L3 },
    { RETRO_DEVICE_ID_JOYPAD_R3,     GWLUA_R3 },
    { RETRO_DEVICE_ID_JOYPAD_SELECT, GWLUA_SELECT },
    { RETRO_DEVICE_ID_JOYPAD_START,  GWLUA_START },
  };
  
  input_poll_cb();
  
  if ( init == 0 )
  {
    /* Initialize game */
    if ( gwlua_create( &state, &rom, perf_cb.get_time_usec() ) )
    {
      log_cb( RETRO_LOG_ERROR, "Error inializing gwlua" );
      init = -1;
      return;
    }
    
    struct retro_system_av_info info;
    retro_get_system_av_info( &info );
    env_cb( RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info );
    
    init = 1;
  }
  else if ( init == -1 )
  {
    /* Error, return */
    return;
  }
  
  /* Run game */
  unsigned id;
  
  for ( id = 0; id < sizeof( map ) / sizeof( map [ 0 ] ); id++ )
  {
    int16_t pressed = input_state_cb( 0, RETRO_DEVICE_JOYPAD, 0, map[ id ].retro );
    gwlua_set_button( &state, map[ id ].gw, pressed != 0 );
  }
  
  gwlua_tick( &state, perf_cb.get_time_usec() );
  
  rl_sprites_blit();
  video_cb( state.screen + offset, soft_width, soft_height, state.width * sizeof( uint16_t ) );
  rl_sprites_unblit();
  
  audio_cb( rl_sound_mix(), RL_SAMPLES_PER_FRAME );
}

void retro_deinit()
{
#ifdef LOG_PERFORMANCE
  perf_cb.perf_log();
#endif
}

void retro_set_controller_port_device( unsigned port, unsigned device )
{
  (void)port;
  (void)device;
}

void retro_reset()
{
  gwlua_reset( &state );
}

size_t retro_serialize_size()
{
  return 0;
}

bool retro_serialize( void* data, size_t size )
{
  (void)data;
  (void)size;
  return false;
}

bool retro_unserialize( const void* data, size_t size )
{
  (void)data;
  (void)size;
  return false;
}

void retro_cheat_reset()
{
}

void retro_cheat_set( unsigned a, bool b, const char* c )
{
  (void)a;
  (void)b;
  (void)c;
}

bool retro_load_game_special(unsigned a, const struct retro_game_info* b, size_t c)
{
  (void)a;
  (void)b;
  (void)c;
  return false;
}

void retro_unload_game()
{
  gwlua_destroy( &state );
  gwrom_destroy( &rom );
}

unsigned retro_get_region()
{
  return RETRO_REGION_NTSC;
}
