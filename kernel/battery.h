#ifndef BATTERY_H
#define BATTERY_H

/* ACPI Control-Method Battery (PNP0C0A) reporting, built on top of the
 * acpi.c table walker and the minimal aml.c AML evaluator. Finds every
 * battery device in the DSDT/SSDT namespace and prints its identity
 * (_BIX, falling back to _BIF), live state (_BST), and two derived
 * numbers: charge percentage (remaining / last-full-charge) and
 * health percentage (last-full-charge / design capacity).
 *
 * Safe to call unconditionally - if ACPI isn't available, no PNP0C0A
 * device exists (e.g. a desktop or a VM with no battery configured),
 * or a method fails to evaluate, this prints a plain status line
 * instead of any of that data. */
void battery_print(void);

#endif
