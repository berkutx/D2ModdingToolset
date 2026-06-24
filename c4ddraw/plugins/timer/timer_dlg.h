// Dialog + control IDs for the timer plugin's ported dialogs (shared by timer.rc and timer.cpp).
// Timetable = the legacy resource-5 Day/Duration grid (sub_100044E0): row 0 is the day-1 base
// (TableDuration_0); rows 1..3 are checkable per-day overrides (TableActive/TableDay/TableDuration_i).
#ifndef TIMER_DLG_H
#define TIMER_DLG_H

#define IDD_TIMETABLE 100
#define IDD_SET 101

#define IDC_SET_SEC 1100 // Set dialog: seconds to set the current turn to (legacy DialogFunc = 1000*value)

#define IDC_TT_DUR0 1000 // day-1 base duration (TableDuration_0)
// rows 1..3 — contiguous so the proc can index by (id - IDC_TT_*1):
#define IDC_TT_ACT1 1011
#define IDC_TT_ACT2 1012
#define IDC_TT_ACT3 1013
#define IDC_TT_DAY1 1021
#define IDC_TT_DAY2 1022
#define IDC_TT_DAY3 1023
#define IDC_TT_DUR1 1031
#define IDC_TT_DUR2 1032
#define IDC_TT_DUR3 1033

#endif // TIMER_DLG_H
