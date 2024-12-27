#ifndef LIBRETRO_CORE_OPTIONS_H__
#define LIBRETRO_CORE_OPTIONS_H__

#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#include <retro_inline.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 ********************************
 * Core Option Definitions
 ********************************
*/

/* RETRO_LANGUAGE_ENGLISH */

/* Default language:
 * - All other languages must include the same keys and values
 * - Will be used as a fallback in the event that frontend language
 *   is not available
 * - Will be used as a fallback for any missing entries in
 *   frontend language definition */

struct retro_core_option_definition option_defs_us[] = {
   {
      "tyrquake_resolution",
      "Internal resolution (restart)",
      "Configure the resolution. Requires a restart.",
      {
         { "320x200",   NULL },
         { "320x240",   NULL },
         { "320x480",   NULL },
         { "360x200",   NULL },
         { "360x240",   NULL },
         { "360x400",   NULL },
         { "360x480",   NULL },
         { "384x216",   NULL },
         { "400x224",   NULL },
         { "400x240",   NULL },
         { "480x272",   NULL },
         { "512x224",   NULL },
         { "512x240",   NULL },
         { "512x384",   NULL },
         { "512x512",   NULL },
         { "640x224",   NULL },
         { "640x240",   NULL },
         { "640x448",   NULL },
         { "640x400",   NULL },
         { "640x480",   NULL },
         { "720x576",   NULL },
         { "800x480",   NULL },
         { "800x600",   NULL },
         { "848x480",   NULL },
         { "960x720",   NULL },
         { "960x600",   NULL },
         { "1024x768",  NULL },
         { "1064x600",  NULL },
         { "1280x720",  NULL },
         { "1280x768",  NULL },
         { "1280x800",  NULL },
         { "1280x960",  NULL },
         { "1280x1024", NULL },
	 { "1360x768",  NULL },
         { "1400x1050", NULL },
         { "1600x900",  NULL },
         { "1600x1000", NULL },
         { "1600x1200", NULL },
         { "1680x1050", NULL },
	 { "1704x960",  NULL },
	 { "1728x1080", NULL },
	 { "1864x1050", NULL },
         { "1920x1080", NULL },
         { "1920x1200", NULL },
         { NULL, NULL },
      },
#if defined(_3DS)
      "400x240"
#elif defined(DINGUX)
      "320x240"
#else
      "320x200"
#endif
   },
   {
      "tyrquake_framerate",
      "Framerate (restart)",
      "Modify framerate. Requires a restart. Values above 72 may cause various timing bugs.",
      {
         { "auto",            "Auto"},
         { "10",              "10fps"},
         { "15",              "15fps"},
         { "20",              "20fps"},
         { "25",              "25fps"},
         { "30",              "30fps"},
         { "40",              "40fps"},
         { "50",              "50fps"},
         { "60",              "60fps"},
         { "72",              "72fps"},
         { "75",              "75fps"},
         { "90",              "90fps"},
         { "100",              "100fps"},
         { "119",              "119fps"},
         { "120",              "120fps"},
         { "144",              "144fps"},
         { "155",              "155fps"},
         { "160",              "160fps"},
         { "165",              "165fps"},
         { "180",              "180fps"},
         { "200",              "200fps"},
         { "240",              "240fps"},
         { "244",              "244fps"},
         { "300",              "300fps"},
         { "320",              "320fps"},
         { "360",              "360fps"},
         { "480",              "480fps"},
         { "540",              "540fps"},
         { NULL, NULL },
      },
#if defined(_3DS)
      "25"
#elif defined(_MIYOO)
      "15"
#elif defined(DINGUX)
      "30"
#else
      "auto"
#endif
   },
   {
      "tyrquake_colored_lighting",
      "Colored lighting (restart)",
      "Enables colored lightning when the loaded content supports it. Requires a restart.",
      {
         { "disabled",              "Disabled"},
         { "enabled",               "Enabled"},
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "tyrquake_rumble",
      "Rumble",
      "Enables joypad rumble.",
      {
         { "disabled",  "Disabled" },
         { "enabled",   "Enabled" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "tyrquake_invert_y_axis",
      "Invert Y Axis",
      "Invert the gamepad right analog stick's Y axis.",
      {
         { "disabled",  "Disabled" },
         { "enabled",   "Enabled" },
         { NULL, NULL },
      },
      "disabled"
   },
   {
      "tyrquake_analog_deadzone",
      "Analog Deadzone (percent)",
      "Sets the deadzone of the Gamepad analog sticks when the input device type is set to 'Gamepad Classic' or 'Gamepad Modern'.",
      {
         { "0", NULL },
         { "5", NULL },
         { "10", NULL },
         { "15",  NULL },
         { "20",  NULL },
         { "25",  NULL },
         { "30",  NULL },
         { NULL, NULL },
      },
      "15"
   },
   { NULL, NULL, NULL, {{0}}, NULL },
};

/* RETRO_LANGUAGE_JAPANESE */

/* RETRO_LANGUAGE_FRENCH */

/* RETRO_LANGUAGE_SPANISH */

/* RETRO_LANGUAGE_GERMAN */

/* RETRO_LANGUAGE_ITALIAN */

/* RETRO_LANGUAGE_DUTCH */

/* RETRO_LANGUAGE_PORTUGUESE_BRAZIL */

/* RETRO_LANGUAGE_PORTUGUESE_PORTUGAL */

/* RETRO_LANGUAGE_RUSSIAN */

/* RETRO_LANGUAGE_KOREAN */

/* RETRO_LANGUAGE_CHINESE_TRADITIONAL */

/* RETRO_LANGUAGE_CHINESE_SIMPLIFIED */

/* RETRO_LANGUAGE_ESPERANTO */

/* RETRO_LANGUAGE_POLISH */

/* RETRO_LANGUAGE_VIETNAMESE */

/* RETRO_LANGUAGE_ARABIC */

/* RETRO_LANGUAGE_GREEK */

/* RETRO_LANGUAGE_TURKISH */

/*
 ********************************
 * Language Mapping
 ********************************
*/

struct retro_core_option_definition *option_defs_intl[RETRO_LANGUAGE_LAST] = {
   option_defs_us, /* RETRO_LANGUAGE_ENGLISH */
   NULL,           /* RETRO_LANGUAGE_JAPANESE */
   NULL,           /* RETRO_LANGUAGE_FRENCH */
   NULL,           /* RETRO_LANGUAGE_SPANISH */
   NULL,           /* RETRO_LANGUAGE_GERMAN */
   NULL,           /* RETRO_LANGUAGE_ITALIAN */
   NULL,           /* RETRO_LANGUAGE_DUTCH */
   NULL,           /* RETRO_LANGUAGE_PORTUGUESE_BRAZIL */
   NULL,           /* RETRO_LANGUAGE_PORTUGUESE_PORTUGAL */
   NULL,           /* RETRO_LANGUAGE_RUSSIAN */
   NULL,           /* RETRO_LANGUAGE_KOREAN */
   NULL,           /* RETRO_LANGUAGE_CHINESE_TRADITIONAL */
   NULL,           /* RETRO_LANGUAGE_CHINESE_SIMPLIFIED */
   NULL,           /* RETRO_LANGUAGE_ESPERANTO */
   NULL,           /* RETRO_LANGUAGE_POLISH */
   NULL,           /* RETRO_LANGUAGE_VIETNAMESE */
   NULL,           /* RETRO_LANGUAGE_ARABIC */
   NULL,           /* RETRO_LANGUAGE_GREEK */
   NULL,           /* RETRO_LANGUAGE_TURKISH */
};

/*
 ********************************
 * Functions
 ********************************
*/

/* Handles configuration/setting of core options.
 * Should only be called inside retro_set_environment().
 * > We place the function body in the header to avoid the
 *   necessity of adding more .c files (i.e. want this to
 *   be as painless as possible for core devs)
 */

static INLINE void libretro_set_core_options(retro_environment_t environ_cb)
{
   unsigned version = 0;

   if (!environ_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) && (version >= 1))
   {
      struct retro_core_options_intl core_options_intl;
      unsigned language = 0;

      core_options_intl.us    = option_defs_us;
      core_options_intl.local = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
          (language < RETRO_LANGUAGE_LAST) && (language != RETRO_LANGUAGE_ENGLISH))
         core_options_intl.local = option_defs_intl[language];

      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL, &core_options_intl);
   }
   else
   {
      size_t i;
      size_t num_options               = 0;
      struct retro_variable *variables = NULL;
      char **values_buf                = NULL;

      /* Determine number of options */
      while (true)
      {
         if (option_defs_us[num_options].key)
            num_options++;
         else
            break;
      }

      /* Allocate arrays */
      variables  = (struct retro_variable *)calloc(num_options + 1, sizeof(struct retro_variable));
      values_buf = (char **)calloc(num_options, sizeof(char *));

      if (!variables || !values_buf)
         goto error;

      /* Copy parameters from option_defs_us array */
      for (i = 0; i < num_options; i++)
      {
         const char *key                        = option_defs_us[i].key;
         const char *desc                       = option_defs_us[i].desc;
         const char *default_value              = option_defs_us[i].default_value;
         struct retro_core_option_value *values = option_defs_us[i].values;
         size_t buf_len                         = 3;
         size_t default_index                   = 0;

         values_buf[i] = NULL;

         if (desc)
         {
            size_t num_values = 0;

            /* Determine number of values */
            while (true)
            {
               if (values[num_values].value)
               {
                  /* Check if this is the default value */
                  if (default_value)
                     if (strcmp(values[num_values].value, default_value) == 0)
                        default_index = num_values;

                  buf_len += strlen(values[num_values].value);
                  num_values++;
               }
               else
                  break;
            }

            /* Build values string */
            if (num_values > 1)
            {
               size_t j;

               buf_len += num_values - 1;
               buf_len += strlen(desc);

               values_buf[i] = (char *)calloc(buf_len, sizeof(char));
               if (!values_buf[i])
                  goto error;

               strcpy(values_buf[i], desc);
               strcat(values_buf[i], "; ");

               /* Default value goes first */
               strcat(values_buf[i], values[default_index].value);

               /* Add remaining values */
               for (j = 0; j < num_values; j++)
               {
                  if (j != default_index)
                  {
                     strcat(values_buf[i], "|");
                     strcat(values_buf[i], values[j].value);
                  }
               }
            }
         }

         variables[i].key   = key;
         variables[i].value = values_buf[i];
      }
      
      /* Set variables */
      environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);

error:

      /* Clean up */
      if (values_buf)
      {
         for (i = 0; i < num_options; i++)
         {
            if (values_buf[i])
            {
               free(values_buf[i]);
               values_buf[i] = NULL;
            }
         }

         free(values_buf);
         values_buf = NULL;
      }

      if (variables)
      {
         free(variables);
         variables = NULL;
      }
   }
}

#ifdef __cplusplus
}
#endif

#endif
