#ifndef AML_H
#define AML_H

#include <stdint.h>
#include "acpi.h"

/* ---- Minimal AML (ACPI Machine Language) engine -----------------------
 *
 * This is NOT a general-purpose ACPI interpreter. It's a small,
 * best-effort namespace indexer + bytecode evaluator sized for one
 * job: finding an ACPI Control-Method Battery device (_HID PNP0C0A)
 * and evaluating its _STA/_BIF/_BIX/_BST methods.
 *
 * What it supports: Scope/Device/Method/Name/OperationRegion/Field
 * namespace objects; integer/string/package constants; Local/Arg
 * variables; Store and the common arithmetic/logic/compare ops;
 * If/Else/While/Break/Continue/Return; zero-argument method calls;
 * Package construction; and field reads/writes against SystemMemory,
 * SystemIO, and EmbeddedControl operation regions.
 *
 * What it does NOT support: Buffers, Mutex/Event/synchronization,
 * RefOf/DerefOf/Index/Match/CondRefOf, CreateXField buffer accessors,
 * string/buffer conversion ops, dynamically Load()-ed tables, and
 * methods that take arguments. Hitting any of these while evaluating
 * a method aborts just that evaluation (reported back as failure) -
 * it will not crash the kernel or hang indexing of the rest of the
 * table. Real DSDTs vary a lot; treat this as "usually enough for
 * simple battery AML", not a guarantee.
 */

enum aml_value_type {
    AML_INTEGER = 0,
    AML_STRING,
    AML_PACKAGE,
    AML_UNINITIALIZED,
};

struct aml_value {
    enum aml_value_type type;
    uint64_t integer;                 /* AML_INTEGER */
    const char* str;                  /* AML_STRING - not NUL-terminated-safe to assume; use str_len */
    uint32_t str_len;
    struct aml_value* pkg_elems;      /* AML_PACKAGE - lives in a scratch pool, valid until next aml_evaluate() */
    uint32_t pkg_count;
};

/* Opaque handle to a namespace node (Device/Name/Method/...). */
struct aml_node;

/* Resets the whole namespace (call once before indexing the DSDT and
 * any SSDTs for a fresh table set) and parses one table's AML body
 * into the namespace tree rooted at "\". Safe to call more than once
 * with different tables (DSDT, then each SSDT) to build one combined
 * namespace - that's the intended use. */
void aml_reset_namespace(void);
void aml_index_table(struct acpi_sdt_header* table);

/* Finds every Device node anywhere in the namespace whose _HID (or
 * _CID) matches the given EISA-style ID string (e.g. "PNP0C0A").
 * Writes up to max_out matches into out[] and returns how many were
 * found. */
int aml_find_devices_by_hid(const char* hid, struct aml_node** out, int max_out);

/* Looks up a single named child directly under a device node (e.g.
 * "_BST", "_STA") - NOT a full path search. Returns NULL if absent. */
struct aml_node* aml_child(struct aml_node* device, const char* name4);

/* Evaluates a Method node with zero arguments (all _STA/_BIF/_BIX/_BST
 * take none), or reads a Name/Field node's value directly. Returns 1
 * on success (result written to *out), 0 if evaluation hit something
 * unsupported. The scratch pool backing any AML_PACKAGE in *out is
 * only valid until the next aml_evaluate() call - copy out any fields
 * you need before calling it again. */
int aml_evaluate(struct aml_node* node, struct aml_value* out);

#endif
