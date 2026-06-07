/* =======================================================================
 * AUTOLOAD PATCH for Genesis-Plus-GX libretro/libretro.c
 *
 * This file contains the THREE EDITS needed to add config-driven
 * auto-load save state support to Genesis-Plus-GX.
 *
 * Key difference from fceumm: Genesis Plus GX uses log_cb as a bare
 * function pointer (retro_log_printf_t), NOT a struct. The autoload
 * block below is adapted accordingly: calls use  log_cb(...)  instead
 * of  log_cb.log(...).
 *
 * Apply these changes to the upstream libretro/libretro.c from:
 *   https://github.com/libretro/Genesis-Plus-GX
 * ======================================================================= */


/* =======================================================================
 * EDIT A — the helper block
 *
 * Insert this entire block BEFORE retro_run() in libretro/libretro.c.
 * In the upstream source, retro_run() starts at approximately line 3869.
 * Place this block right before that function.
 * ======================================================================= */

/* ---- Config-driven auto-load save state (cross-platform) ------------ *
 * On the first frame after a ROM loads, the core reads a config file that
 * sits next to it and is named after it: e.g. "genesis_plus_gx_libretro.cfg"
 * beside "genesis_plus_gx_libretro.dll" (Windows) or
 * "genesis_plus_gx_libretro_android.cfg" beside
 * "genesis_plus_gx_libretro_android.so" (Android). If enabled, it loads:
 *
 *     <save_path>/<rom-name-without-extension>.<state_ext>
 *
 * Example config (genesis_plus_gx_libretro.cfg):
 *     enabled   = 1
 *     save_path = G:\RetroBat\saves\megadrive\libretro.genplusgx
 *     save_path = G:\RetroBat\saves\mastersystem\libretro.genplusgx
 *     save_path = G:\RetroBat\saves\gamegear\libretro.genplusgx
 *     state_ext = state.auto
 *
 * A line "[autoload] ..." is written both to the RetroArch log and to
 * "autoload.log" beside the core, so problems are easy to diagnose.       */

#ifdef _WIN32
#include <windows.h>
#include <time.h>
#define AUTOLOAD_STRCASECMP _stricmp
#define AUTOLOAD_MAX_PATH   MAX_PATH
#define AUTOLOAD_PATH_SEP   '\\'
#else
#include <dlfcn.h>
#include <time.h>
#include <strings.h>
#define AUTOLOAD_STRCASECMP strcasecmp
#define AUTOLOAD_MAX_PATH   4096
#define AUTOLOAD_PATH_SEP   '/'
#endif

#define AUTOLOAD_MAX_RUNS 25   /* keep only the latest N runs in autoload.log */
#define AUTOLOAD_MAX_PATHS 16  /* max number of save_path entries in the config */

static bool autoload_state_pending      = false;
static char autoload_dir[AUTOLOAD_MAX_PATH]      = {0};   /* folder the core lives in */
static char autoload_rom_path[AUTOLOAD_MAX_PATH] = {0};   /* full path of the loaded ROM   */
static char autoload_core_name[64]      = {0};   /* this core's file name, no ext */

/* Directory that THIS core (.dll/.so) lives in. */
static void autoload_get_self_dir(char *out, size_t out_size)
{
   out[0] = '\0';
#ifdef _WIN32
   HMODULE hm = NULL;
   char path[AUTOLOAD_MAX_PATH];
   char *slash;
   if (!GetModuleHandleExA(
         GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
         (LPCSTR)&autoload_get_self_dir, &hm))
      return;
   if (!GetModuleFileNameA(hm, path, sizeof(path)))
      return;
   slash = strrchr(path, '\\');
   if (slash) *slash = '\0';
   strncpy(out, path, out_size - 1);
   out[out_size - 1] = '\0';
#else
   Dl_info info;
   if (dladdr((const void *)&autoload_get_self_dir, &info) && info.dli_fname)
   {
      char *slash;
      strncpy(out, info.dli_fname, out_size - 1);
      out[out_size - 1] = '\0';
      slash = strrchr(out, '/');
      if (slash) *slash = '\0';
   }
#endif
}

/* This core's own file name, without directory or extension
 * (e.g. "genesis_plus_gx_libretro" or "genesis_plus_gx_libretro_android"). */
static void autoload_get_self_name(char *out, size_t out_size)
{
   out[0] = '\0';
#ifdef _WIN32
   HMODULE hm = NULL;
   char path[AUTOLOAD_MAX_PATH];
   char *slash, *dot, *base;
   if (!GetModuleHandleExA(
         GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
         (LPCSTR)&autoload_get_self_name, &hm))
      return;
   if (!GetModuleFileNameA(hm, path, sizeof(path)))
      return;
   slash = strrchr(path, '\\');
   base  = slash ? slash + 1 : path;
   strncpy(out, base, out_size - 1);
   out[out_size - 1] = '\0';
   dot = strrchr(out, '.');
   if (dot) *dot = '\0';
#else
   Dl_info info;
   if (dladdr((const void *)&autoload_get_self_name, &info) && info.dli_fname)
   {
      char *slash, *dot, *base;
      strncpy(out, info.dli_fname, out_size - 1);
      out[out_size - 1] = '\0';
      slash = strrchr(out, '/');
      base  = slash ? slash + 1 : out;
      if (base != out) memmove(out, base, strlen(base) + 1);
      dot = strrchr(out, '.');
      if (dot) *dot = '\0';
   }
#endif
}

/* Build the config file path: <core_dir>/<core_name_without_ext>.cfg */
static void autoload_get_self_cfg(char *out, size_t out_size)
{
   char name[64];
   out[0] = '\0';
   autoload_get_self_name(name, sizeof(name));
   if (name[0] == '\0')
      return;
   if (autoload_dir[0] != '\0')
      snprintf(out, out_size, "%s%c%s.cfg", autoload_dir, AUTOLOAD_PATH_SEP, name);
   else
      snprintf(out, out_size, "%s.cfg", name);
}

/* Write a line to autoload.log next to the core AND to the RetroArch log.
 * NOTE: Genesis Plus GX uses log_cb as a bare function pointer
 * (retro_log_printf_t), so we call log_cb(...) directly. */
static void autoload_logf(const char *fmt, ...)
{
   char    line[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(line, sizeof(line), fmt, ap);
   va_end(ap);

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "[autoload] %s\n", line);

   if (autoload_dir[0] != '\0')
   {
      char logpath[AUTOLOAD_MAX_PATH + 32];
      FILE *fp;
      snprintf(logpath, sizeof(logpath), "%s%cautoload.log", autoload_dir, AUTOLOAD_PATH_SEP);
      fp = fopen(logpath, "a");
      if (fp) { fprintf(fp, "%s\n", line); fclose(fp); }
   }
}

/* Trim autoload.log so it keeps only the most recent `keep_runs` runs.
 * Runs are delimited by lines beginning with "--- autoload run ---". */
static void autoload_log_rotate(int keep_runs)
{
   char        logpath[AUTOLOAD_MAX_PATH + 32];
   const char *marker = "--- autoload run ---";
   FILE       *fp;
   long        fsize;
   char       *data, *p, *cut;
   int         count = 0, seen = 0;
   size_t      got;

   if (autoload_dir[0] == '\0')
      return;
   snprintf(logpath, sizeof(logpath), "%s%cautoload.log", autoload_dir, AUTOLOAD_PATH_SEP);

   fp = fopen(logpath, "rb");
   if (!fp)
      return;                       /* no log yet */
   fseek(fp, 0, SEEK_END);
   fsize = ftell(fp);
   fseek(fp, 0, SEEK_SET);
   if (fsize <= 0) { fclose(fp); return; }
   data = (char*)malloc((size_t)fsize + 1);
   if (!data) { fclose(fp); return; }
   got = fread(data, 1, (size_t)fsize, fp);
   data[got] = '\0';
   fclose(fp);

   p = data;
   while ((p = strstr(p, marker)) != NULL) { count++; p += 1; }

   if (count > keep_runs)
   {
      int drop = count - keep_runs; /* leading runs to remove */
      cut = data;
      p   = data;
      while ((p = strstr(p, marker)) != NULL)
      {
         if (++seen == drop + 1) { cut = p; break; }
         p += 1;
      }
      fp = fopen(logpath, "wb");
      if (fp) { fwrite(cut, 1, strlen(cut), fp); fclose(fp); }
   }
   free(data);
}

/* ---- tiny "key = value" config parser ------------------------------- */
typedef struct
{
   bool enabled;
   int  num_paths;
   char save_paths[AUTOLOAD_MAX_PATHS][AUTOLOAD_MAX_PATH];
   char state_ext[64];
} autoload_config;

static void autoload_trim(char *s)
{
   size_t len;
   char  *start = s;
   while (*start == ' ' || *start == '\t') start++;
   if (start != s) memmove(s, start, strlen(start) + 1);
   len = strlen(s);
   while (len > 0 && (s[len-1] == '\r' || s[len-1] == '\n' ||
                      s[len-1] == ' '  || s[len-1] == '\t'))
      s[--len] = '\0';
}

static bool autoload_read_config(const char *cfg_path, autoload_config *cfg)
{
   FILE *fp;
   char  line[1024];

   cfg->enabled      = false;
   cfg->num_paths    = 0;
   strcpy(cfg->state_ext, "state.auto");   /* default extension */

   fp = fopen(cfg_path, "r");
   if (!fp)
      return false;

   while (fgets(line, sizeof(line), fp))
   {
      char *eq, *key, *val;
      if (line[0] == '#' || line[0] == ';')
         continue;
      eq = strchr(line, '=');
      if (!eq)
         continue;
      *eq = '\0';
      key = line;
      val = eq + 1;
      autoload_trim(key);
      autoload_trim(val);

      if (AUTOLOAD_STRCASECMP(key, "enabled") == 0)
         cfg->enabled = (AUTOLOAD_STRCASECMP(val, "1")    == 0 ||
                         AUTOLOAD_STRCASECMP(val, "true") == 0 ||
                         AUTOLOAD_STRCASECMP(val, "yes")  == 0 ||
                         AUTOLOAD_STRCASECMP(val, "on")   == 0);
      else if (AUTOLOAD_STRCASECMP(key, "save_path") == 0)
      {
         if (cfg->num_paths < AUTOLOAD_MAX_PATHS && *val)
         {
            strncpy(cfg->save_paths[cfg->num_paths], val, AUTOLOAD_MAX_PATH - 1);
            cfg->save_paths[cfg->num_paths][AUTOLOAD_MAX_PATH - 1] = '\0';
            cfg->num_paths++;
         }
      }
      else if (AUTOLOAD_STRCASECMP(key, "state_ext") == 0)
      {
         if (*val == '.') val++;            /* accept "state" or ".state" */
         if (*val)
         {
            strncpy(cfg->state_ext, val, sizeof(cfg->state_ext) - 1);
            cfg->state_ext[sizeof(cfg->state_ext) - 1] = '\0';
         }
      }
   }
   fclose(fp);
   return true;
}

/* ROM file name, without directory and without extension.
 * Handles archive content where the frontend passes the path as
 * "<archive>.zip#<inner-file>.md" - we use the inner file name. */
static void autoload_rom_basename(char *out, size_t out_size)
{
   const char *p    = autoload_rom_path;
   const char *hash = strrchr(p, '#');   /* archive separator, if any */
   const char *base, *s1, *s2;
   char *dot;

   if (hash)
      p = hash + 1;                       /* skip "<archive>.zip#" */

   base = p;
   s1   = strrchr(p, '\\');
   s2   = strrchr(p, '/');
   if (s1 && (!s2 || s1 > s2)) base = s1 + 1;
   else if (s2)                base = s2 + 1;

   strncpy(out, base, out_size - 1);
   out[out_size - 1] = '\0';
   dot = strrchr(out, '.');
   if (dot) *dot = '\0';
}

/* RetroArch wraps states in a "RASTATE" container; the real data is in the
 * "MEM " block. Returns true (and points at the MEM block) if it's a
 * container, false to treat the file as a raw state. */
static bool autoload_extract_rastate(const uint8_t *file, size_t file_len,
                                     const uint8_t **out_data, size_t *out_size)
{
   size_t pos;
   if (file_len < 8 || memcmp(file, "RASTATE", 7) != 0)
      return false;
   pos = 8;
   while (pos + 8 <= file_len)
   {
      const uint8_t *p = file + pos;
      uint32_t blocksize =  (uint32_t)p[4]
                         | ((uint32_t)p[5] << 8)
                         | ((uint32_t)p[6] << 16)
                         | ((uint32_t)p[7] << 24);
      pos += 8;
      if (memcmp(p, "MEM ", 4) == 0)
      {
         if (pos + blocksize > file_len) return false;
         *out_data = file + pos;
         *out_size = (size_t)blocksize;
         return true;
      }
      if (memcmp(p, "END ", 4) == 0)
         break;
      pos += blocksize;
   }
   return false;
}

static void autoload_try_load_state(void)
{
   autoload_config cfg;
   char            cfg_path[AUTOLOAD_MAX_PATH];
   char            romname[AUTOLOAD_MAX_PATH];
   char            file[AUTOLOAD_MAX_PATH * 3];
   void           *buf        = NULL;
   int64_t         len        = 0;
   size_t          expected   = retro_serialize_size();
   const uint8_t  *state_data = NULL;
   size_t          state_size = 0;
   size_t          plen;

   autoload_get_self_dir(autoload_dir, sizeof(autoload_dir));
   if (autoload_dir[0] == '\0')
   {
      /* NOTE: Genesis Plus GX uses log_cb as a bare function pointer */
      if (log_cb)
         log_cb(RETRO_LOG_WARN,
               "[autoload] could not resolve core directory\n");
      return;
   }

   autoload_get_self_name(autoload_core_name, sizeof(autoload_core_name));
   autoload_log_rotate(AUTOLOAD_MAX_RUNS - 1);  /* keep 24; this run makes 25 */

   {
      time_t     t  = time(NULL);
      struct tm *lt = localtime(&t);
      char       ts[32];
      if (lt) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);
      else    ts[0] = '\0';
      autoload_logf("--- autoload run --- core=%s  %s", autoload_core_name, ts);
   }
   autoload_logf("core directory : %s", autoload_dir);

   autoload_get_self_cfg(cfg_path, sizeof(cfg_path));
   autoload_logf("config file   : %s", cfg_path);

   if (!autoload_read_config(cfg_path, &cfg))
   {
      autoload_logf("RESULT        : config file not found - nothing loaded");
      return;
   }

   if (!cfg.enabled)
   {
      autoload_logf("RESULT        : disabled in config (enabled=0)");
      return;
   }

   if (cfg.num_paths == 0)
   {
      autoload_logf("RESULT        : no save_path in config");
      return;
   }

   if (autoload_rom_path[0] == '\0')
   {
      autoload_logf("RESULT        : ROM path unknown (frontend gave none)");
      return;
   }

   autoload_rom_basename(romname, sizeof(romname));
   autoload_logf("rom path      : %s", autoload_rom_path);
   autoload_logf("rom name      : %s", romname);
   autoload_logf("state ext     : %s", cfg.state_ext);
   autoload_logf("core expects  : %u bytes", (unsigned)expected);

   /* Try each save_path in order; the first folder that has a matching
    * state file wins. (Essential for multi-system cores like Genesis Plus GX
    * that store states in per-system folders.) */
   {
      int  i;
      bool found = false;
      for (i = 0; i < cfg.num_paths; i++)
      {
         char  *sp = cfg.save_paths[i];
         while ((plen = strlen(sp)) > 0 &&
                (sp[plen-1] == '\\' || sp[plen-1] == '/'))
            sp[plen-1] = '\0';

         snprintf(file, sizeof(file), "%s%c%s.%s", sp, AUTOLOAD_PATH_SEP, romname, cfg.state_ext);
         autoload_logf("trying        : %s", file);

         if (filestream_read_file(file, &buf, &len))
         {
            autoload_logf("found in      : %s", sp);
            found = true;
            break;
         }
      }
      if (!found)
      {
         autoload_logf("RESULT        : no matching state file in any save_path");
         return;
      }
   }

   autoload_logf("file size     : %d bytes", (int)len);

   if (buf && len > 0)
   {
      /* Unwrap RetroArch's RASTATE container if present. */
      if (autoload_extract_rastate((const uint8_t*)buf, (size_t)len,
                                   &state_data, &state_size))
         autoload_logf("RASTATE       : container detected, MEM block = %u bytes",
                       (unsigned)state_size);
      else
      {
         state_data = (const uint8_t*)buf;
         state_size = (size_t)len;
         autoload_logf("RASTATE       : not a container, using raw file");
      }

      if (state_size != expected)
         autoload_logf("WARNING       : state is %u bytes but core expects %u "
                       "(wrong ROM, or RetroArch 'Save State Compression' is ON "
                       "- turn it OFF and re-save).",
                       (unsigned)state_size, (unsigned)expected);

      if (retro_unserialize(state_data, state_size))
         autoload_logf("RESULT        : state loaded OK");
      else
         autoload_logf("RESULT        : retro_unserialize() refused the data");
   }
   else
      autoload_logf("RESULT        : file was empty");

   if (buf)
      free(buf);
}

/* ---- End of Edit A -------------------------------------------------- */


/* =======================================================================
 * EDIT B — trigger on the first frame (inside retro_run)
 *
 * Find the top of retro_run() in libretro/libretro.c (approximately line 3869)
 * and add the autoload check AFTER the local variable declarations:
 *
 *   void retro_run(void)
 *   {
 *      bool okay = false;
 *      int result = -1;
 *      int do_skip = 0;
 *      bool updated = false;
 *      int soundbuffer_size = 0;
 *
 *      // >>> INSERT THESE 4 LINES <<<
 *      if (autoload_state_pending)
 *      {
 *         autoload_state_pending = false;
 *         autoload_try_load_state();
 *      }
 *
 *      is_running = true;
 *      ...
 *   }
 * ======================================================================= */


/* =======================================================================
 * EDIT C — capture the ROM path (end of retro_load_game)
 *
 * Find the success "return true;" at the end of retro_load_game()
 * (approximately line 3690). It is preceded by:
 *
 *      set_memory_maps();
 *      init_frameskip();
 *      return true;
 *
 * Insert BEFORE "return true;":
 *
 *   set_memory_maps();
 *   init_frameskip();
 *
 *   // >>> INSERT THESE 6 LINES <<<
 *   autoload_rom_path[0] = '\0';
 *   if (info && info->path)
 *   {
 *      strncpy(autoload_rom_path, info->path, sizeof(autoload_rom_path) - 1);
 *      autoload_rom_path[sizeof(autoload_rom_path) - 1] = '\0';
 *   }
 *   autoload_state_pending = true;
 *
 *   return true;
 *
 * Note: "info" is the "const struct retro_game_info *info" parameter of
 * retro_load_game(). Do NOT insert before the error-handling "return false;"
 * that follows the "error:" label.
 * ======================================================================= */
