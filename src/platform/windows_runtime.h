#ifndef GHM_WINDOWS_RUNTIME_H
#define GHM_WINDOWS_RUNTIME_H
/* Locate bundled resources relative to the executable, even after moving the
 * extracted package. Only the GUI needs GTK's resource and loader setup. */
int ghm_windows_gui_runtime(void);
#endif
