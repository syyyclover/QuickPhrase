#include "app.h"

int qp_settings_commit(const Settings *candidate, QpSettingsResult *result)
{
    Settings old;
    int old_auto, changed_key, changed_theme = 0, changed_auto = 0;
    QpSettingsResult local = {0};
    QpAutostartSnapshot startup = {0};
    if (!result) result = &local;
    result->failures = result->rollback_failures = 0;
    if (!candidate) { result->failures = QP_SETTINGS_SAVE; return 0; }
    old = g_settings;
    if (!qp_autostart_capture(&startup)) { result->failures = QP_SETTINGS_AUTOSTART; return 0; }
    old_auto = startup.enabled;
    changed_key = old.hotkey_mods != candidate->hotkey_mods || old.hotkey_vk != candidate->hotkey_vk;
    g_settings = *candidate;
    if (changed_key) {
        app_unregister_hotkey();
        if (candidate->hotkey_vk && !app_register_hotkey()) {
            result->failures |= QP_SETTINGS_HOTKEY;
            goto rollback;
        }
    }
    if (old.theme != candidate->theme) {
        if (!app_apply_theme()) {
            result->failures |= QP_SETTINGS_THEME;
            goto rollback;
        }
        changed_theme = 1;
    }
    if (old_auto != candidate->autostart) {
        changed_auto = 1;
        if (!app_set_autostart(candidate->autostart)) {
            result->failures |= QP_SETTINGS_AUTOSTART;
            goto rollback;
        }
    }
    g_settings.autostart = changed_auto ? candidate->autostart : old_auto;
    if (!settings_save(&g_settings)) {
        result->failures |= QP_SETTINGS_SAVE;
        goto rollback;
    }
    qp_autostart_dispose(&startup);
    return 1;

rollback:
    g_settings = old;
    if (changed_key) {
        app_unregister_hotkey();
        if (old.hotkey_vk && !app_register_hotkey()) {
            result->rollback_failures |= QP_SETTINGS_HOTKEY;
            g_settings.hotkey_vk = g_settings.hotkey_mods = 0;
        }
    }
    if (changed_auto && !qp_autostart_restore(&startup)) {
        result->rollback_failures |= QP_SETTINGS_AUTOSTART;
        QpAutostartSnapshot actual = {0};
        if (qp_autostart_capture(&actual)) {
            g_settings.autostart = actual.enabled;
            qp_autostart_dispose(&actual);
        }
    } else g_settings.autostart = old_auto;
    if (changed_theme && !app_apply_theme()) {
        result->rollback_failures |= QP_SETTINGS_THEME;
        g_settings.theme = g_dark ? THEME_DARK : THEME_LIGHT;
    }
    qp_autostart_dispose(&startup);
    return 0;
}
