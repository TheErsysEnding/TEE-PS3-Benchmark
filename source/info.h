/* The main menu's entries and the info page of each (TRIANGLE). */
#ifndef INFO_H
#define INFO_H

enum { M_FLOAT, M_INT, M_MEM, M_ALL, M_SERIES, M_ENDURANCE, M_FULLLOAD, M_RESULTS, M_ENTRIES };

/* the card's short name, the info page's title, the card's line */
const char *menu_name(int entry);
const char *menu_title(int entry);
const char *menu_desc(int entry);

/* the info page: up to a NULL; "#" starts a heading, any other string is a paragraph */
const char *const *info_lines(int entry);

#endif
