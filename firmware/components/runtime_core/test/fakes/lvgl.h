#ifndef LVGL_H
#define LVGL_H

#include <stdbool.h>

typedef struct lv_display_t lv_display_t;
typedef struct lv_obj_t lv_obj_t;

lv_display_t *lv_display_get_default(void);
void lv_display_set_default(lv_display_t *display);
lv_display_t *lv_obj_get_display(const lv_obj_t *object);
void lv_screen_load(lv_obj_t *object);
lv_obj_t *lv_display_get_screen_active(lv_display_t *display);
void lv_obj_delete(lv_obj_t *object);
void lv_obj_clean(lv_obj_t *object);

#endif
