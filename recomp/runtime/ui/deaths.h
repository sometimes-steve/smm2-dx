// Deaths per course (issue #33): how many times the player has died in each course, on this device only. Kept in course-deaths.txt in the
// data folder ("<course data id> <deaths>" per line), written at every death. A course is told apart by its data id (the number in OpenCourse
// World's /level_data/<id>), which is what the game keeps for the course it is playing.
#pragma once
#include <stdint.h>

void deaths_add(uint32_t course);       // a death in this course
unsigned deaths_get(uint32_t course);   // deaths so far in this course (0: none, or never played)
