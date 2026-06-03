/* Standalone tester: dlopen kwin.so and exercise the lxhotkey backend against
 * the real ~/.config/kglobalshortcutsrc, without installing system-wide.
 * Build: see the cc line in the comment below. */
#include <lxhotkey/lxhotkey.h>
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    void *h = dlopen(argv[1] ? argv[1] : "./build/lxhotkey/kwin.so", RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

    LXHotkeyPluginInit *p = dlsym(h, "fm_module_init_lxhotkey");
    if (!p) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 2; }

    GError *err = NULL;
    gpointer cfg = p->load(NULL, &err);
    if (!cfg) { fprintf(stderr, "load failed: %s\n", err ? err->message : "?"); return 1; }

    GList *acts = p->get_wm_actions(cfg, NULL);
    printf("== get_wm_actions: %u available KWin actions ==\n", g_list_length(acts));

    GList *keys = p->get_wm_keys(cfg, NULL, NULL), *l;
    printf("== get_wm_keys: %u BOUND shortcuts ==\n", g_list_length(keys));
    int i = 0;
    for (l = keys; l && i < 12; l = l->next, i++) {
        LXHotkeyGlobal *g = l->data;
        LXHotkeyAttr *a = g->actions->data;
        printf("  %-34s %s%s%s\n", a->name, g->accel1,
               g->accel2 ? " / " : "", g->accel2 ? g->accel2 : "");
    }
    if (g_list_length(keys) > 12) printf("  ... (%u more)\n", g_list_length(keys) - 12);
    g_list_free(keys);

    p->free(cfg);
    return 0;
}
