#!/usr/bin/env python3
"""Exercise production label refresh and scroller selection at LVGL boundaries."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / 'components/nightglass_ui/src/shell.cpp').read_text()
def extract(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]
program = r'''
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
struct lv_obj_t { std::string text; unsigned writes = 0; bool scrollable = false; int direction = 0; std::vector<lv_obj_t*> children; };
constexpr int LV_OBJ_FLAG_SCROLLABLE = 1, LV_DIR_VER = 2;
const char *lv_label_get_text(lv_obj_t *o) { return o->text.c_str(); }
void lv_label_set_text(lv_obj_t *o, const char *s) { ++o->writes; o->text = s ? s : ""; }
unsigned lv_obj_get_child_count(lv_obj_t *o) { return o->children.size(); }
lv_obj_t *lv_obj_get_child(lv_obj_t *o, unsigned i) { return o->children.at(i); }
bool lv_obj_has_flag(lv_obj_t *o, int) { return o->scrollable; }
int lv_obj_get_scroll_dir(lv_obj_t *o) { return o->direction; }
'''
program += extract('void set_label_if_changed(') + '\n' + extract('lv_obj_t *route_scroller(')
program += r'''
int main() {
 lv_obj_t value; value.text = "05:00";
 for (int i=0;i<1000;++i) set_label_if_changed(&value,"05:00");
 assert(value.writes == 0);
 set_label_if_changed(&value,"04:59"); assert(value.writes == 1 && value.text == "04:59");
 set_label_if_changed(&value, ""); assert(value.writes == 2 && value.text.empty());
 set_label_if_changed(nullptr,"ignored");
 lv_obj_t host, button, horizontal, list;
 horizontal.scrollable=true; horizontal.direction=1;
 list.scrollable=true; list.direction=LV_DIR_VER;
 host.children={&button,&horizontal,&list};
 assert(route_scroller(&host)==&list);
 host.children={&button,&horizontal}; assert(route_scroller(&host)==nullptr);
}
'''
with tempfile.TemporaryDirectory() as d:
    path = Path(d)
    (path/'test.cpp').write_text(program)
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('Production UI refresh efficiency passed: 1000 unchanged updates, changes, empty/null and scroller selection')
