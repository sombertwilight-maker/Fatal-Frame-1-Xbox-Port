#ifndef FF_LAUNCHER_H
#define FF_LAUNCHER_H
extern char g_ff_root[];
/* Find/extract/verify the player's disc image and set runtime defaults.
 * Exits the process with a message box on failure. */
void ff_prepare(const char *cmdline);
/* Start the host thread that keeps the GPU model pointed at D3D's fence. */
void ff_gpu_glue_start(void);
#endif
