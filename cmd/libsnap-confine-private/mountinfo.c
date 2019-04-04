/*
 * Copyright (C) 2016 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mountinfo.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleanup-funcs.h"

/**
 * Parse a single mountinfo entry (line).
 *
 * The format, described by Linux kernel documentation, is as follows:
 *
 * 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
 * (1)(2)(3)   (4)   (5)      (6)      (7)   (8) (9)   (10)         (11)
 *
 * (1) mount ID:  unique identifier of the mount (may be reused after umount)
 * (2) parent ID:  ID of parent (or of self for the top of the mount tree)
 * (3) major:minor:  value of st_dev for files on filesystem
 * (4) root:  root of the mount within the filesystem
 * (5) mount point:  mount point relative to the process's root
 * (6) mount options:  per mount options
 * (7) optional fields:  zero or more fields of the form "tag[:value]"
 * (8) separator:  marks the end of the optional fields
 * (9) filesystem type:  name of filesystem of the form "type[.subtype]"
 * (10) mount source:  filesystem specific information or "none"
 * (11) super options:  per super block options
 **/
static sc_mountinfo_entry *sc_parse_mountinfo_entry(const char *line)
    __attribute__((nonnull(1)));

/**
 * Free a sc_mountinfo structure and all its entries.
 **/
static void sc_free_mountinfo(sc_mountinfo * info)
    __attribute__((nonnull(1)));

/**
 * Free a sc_mountinfo entry.
 **/
static void sc_free_mountinfo_entry(sc_mountinfo_entry * entry)
    __attribute__((nonnull(1)));

sc_mountinfo_entry *sc_first_mountinfo_entry(sc_mountinfo * info)
{
	return info->first;
}

sc_mountinfo_entry *sc_next_mountinfo_entry(sc_mountinfo_entry * entry)
{
	return entry->next;
}

sc_mountinfo *sc_parse_mountinfo(const char *fname)
{
	sc_mountinfo *info = calloc(1, sizeof *info);
	if (info == NULL) {
		return NULL;
	}
	if (fname == NULL) {
		fname = "/proc/self/mountinfo";
	}
	FILE *f SC_CLEANUP(sc_cleanup_file) = NULL;
	f = fopen(fname, "rt");
	if (f == NULL) {
		free(info);
		return NULL;
	}
	char *line SC_CLEANUP(sc_cleanup_string) = NULL;
	size_t line_size = 0;
	sc_mountinfo_entry *entry, *last = NULL;
	for (;;) {
		errno = 0;
		if (getline(&line, &line_size, f) == -1) {
			if (errno != 0) {
				sc_free_mountinfo(info);
				return NULL;
			}
			break;
		};
		entry = sc_parse_mountinfo_entry(line);
		if (entry == NULL) {
			sc_free_mountinfo(info);
			return NULL;
		}
		if (last != NULL) {
			last->next = entry;
		} else {
			info->first = entry;
		}
		last = entry;
	}
	return info;
}

static void show_buffers(const char *line, int offset,
			 sc_mountinfo_entry * entry)
{
#ifdef MOUNTINFO_DEBUG
	fprintf(stderr, "Input buffer (first), with offset arrow\n");
	fprintf(stderr, "Output buffer (second)\n");

	fputc(' ', stderr);
	for (int i = 0; i < offset - 1; ++i)
		fputc('-', stderr);
	fputc('v', stderr);
	fputc('\n', stderr);

	fprintf(stderr, ">%s<\n", line);

	fputc('>', stderr);
	for (int i = 0; i < strlen(line); ++i) {
		int c = entry->line_buf[i];
		fputc(c == 0 ? '@' : c == 1 ? '#' : c, stderr);
	}
	fputc('<', stderr);
	fputc('\n', stderr);

	fputc('>', stderr);
	for (int i = 0; i < strlen(line); ++i)
		fputc('=', stderr);
	fputc('<', stderr);
	fputc('\n', stderr);
#endif				// MOUNTINFO_DEBUG
}

static char *parse_next_string_field(sc_mountinfo_entry * entry,
				     const char *line, int *offset)
{
	int offset_delta = 0;
	char *field = &entry->line_buf[0] + *offset;
	if (line[*offset] == ' ') {
		// Special case for empty fields which cannot be parsed with %s.
		*field = '\0';
		*offset += 1;
	} else {
		int nscanned =
		    sscanf(line + *offset, "%s%n", field, &offset_delta);
		if (nscanned != 1)
			return NULL;
		*offset += offset_delta;
		if (line[*offset] == ' ') {
			*offset += 1;
		}
	}
	show_buffers(line, *offset, entry);
	return field;
}

static sc_mountinfo_entry *sc_parse_mountinfo_entry(const char *line)
{
	// NOTE: the sc_mountinfo structure is allocated along with enough extra
	// storage to hold the whole line we are parsing. This is used as backing
	// store for all text fields.
	//
	// The idea is that since the line has a given length and we are only after
	// set of substrings we can easily predict the amount of required space
	// (after all, it is just a set of non-overlapping substrings) and append
	// it to the allocated entry structure.
	//
	// The parsing code below, specifically parse_next_string_field(), uses
	// this extra memory to hold data parsed from the original line. In the
	// end, the result is similar to using strtok except that the source and
	// destination buffers are separate.
	//
	// At the end of the parsing process, the input buffer (line) and the
	// output buffer (entry->line_buf) are the same except for where spaces
	// were converted into NUL bytes (string terminators) and except for the
	// leading part of the buffer that contains mount_id, parent_id, dev_major
	// and dev_minor integer fields that are parsed separately.
	//
	// If MOUNTINFO_DEBUG is defined then extra debugging is printed to stderr
	// and this allows for visual analysis of what is going on.
	sc_mountinfo_entry *entry = calloc(1, sizeof *entry + strlen(line) + 1);
	if (entry == NULL) {
		return NULL;
	}
#ifdef MOUNTINFO_DEBUG
	// Poison the buffer with '\1' bytes that are printed as '#' characters
	// by show_buffers() below. This is "unaltered" memory.
	memset(entry->line_buf, 1, strlen(line));
#endif				// MOUNTINFO_DEBUG
	int nscanned;
	int offset_delta, offset = 0;
	nscanned = sscanf(line, "%d %d %u:%u %n",
			  &entry->mount_id, &entry->parent_id,
			  &entry->dev_major, &entry->dev_minor, &offset_delta);
	if (nscanned != 4)
		goto fail;
	offset += offset_delta;

	show_buffers(line, offset, entry);

	if ((entry->root =
	     parse_next_string_field(entry, line, &offset)) == NULL)
		goto fail;
	if ((entry->mount_dir =
	     parse_next_string_field(entry, line, &offset)) == NULL)
		goto fail;
	if ((entry->mount_opts =
	     parse_next_string_field(entry, line, &offset)) == NULL)
		goto fail;
	entry->optional_fields = &entry->line_buf[0] + offset;
	// NOTE: This ensures that optional_fields is never NULL. If this changes,
	// must adjust all callers of parse_mountinfo_entry() accordingly.
	for (int field_num = 0;; ++field_num) {
		char *opt_field = parse_next_string_field(entry, line, &offset);
		if (opt_field == NULL)
			goto fail;
		if (strcmp(opt_field, "-") == 0) {
			opt_field[0] = 0;
			break;
		}
		if (field_num > 0) {
			opt_field[-1] = ' ';
		}
	}
	if ((entry->fs_type =
	     parse_next_string_field(entry, line, &offset)) == NULL)
		goto fail;
	if ((entry->mount_source =
	     parse_next_string_field(entry, line, &offset)) == NULL)
		goto fail;
	if ((entry->super_opts =
	     parse_next_string_field(entry, line, &offset)) == NULL)
		goto fail;
	show_buffers(line, offset, entry);
	return entry;
 fail:
	free(entry);
	return NULL;
}

void sc_cleanup_mountinfo(sc_mountinfo ** ptr)
{
	if (*ptr != NULL) {
		sc_free_mountinfo(*ptr);
		*ptr = NULL;
	}
}

static void sc_free_mountinfo(sc_mountinfo * info)
{
	sc_mountinfo_entry *entry, *next;
	for (entry = info->first; entry != NULL; entry = next) {
		next = entry->next;
		sc_free_mountinfo_entry(entry);
	}
	free(info);
}

static void sc_free_mountinfo_entry(sc_mountinfo_entry * entry)
{
	free(entry);
}
