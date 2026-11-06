"""Patches to hactool 1.4.0's source for this project (tools/dx-toolchain.ps1 applies them before compiling).

    python hactool_patch.py <hactool source dir>
"""
import sys

src = sys.argv[1]

# Fixes to 1.4.0 (nca.c):
# - an update's (BKTR) relocation and subsection tables are spread out in place with memcpy over overlapping memory:
#   Apple's and glibc's memcpy happen to cope, Windows' copies forwards and scrambles the tables, so applying an
#   update crashed there (0xC0000005 in bktr_get_relocation). memmove.
# - reads of fewer than 16 bytes inside one AES block: in a patched RomFS the bytes came from the stale sector_ofs
#   instead of this read's block offset; in plain CTR sections the next seek dropped the bytes just read (upstream's
#   fix #98, after 1.4.0)
def patch_nca():
    p = src + "/nca.c"
    import sys
    s = open(p).read()
    s = s.replace('memcpy(bktr_get_relocation_bucket(', 'memmove(bktr_get_relocation_bucket(')
    s = s.replace('memcpy(bktr_get_subsection_bucket(', 'memmove(bktr_get_subsection_bucket(')
    assert s.count('memmove(bktr_get_') == 2
    fixes = [
        ('                memcpy(buffer, block_buf + ctx->sector_ofs, count);\n'
         '                nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);',
         '                memcpy(buffer, block_buf + block_ofs, count);\n'
         '                nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);'),
        ('                    nca_section_fseek(ctx, ctx->cur_seek - ctx->offset);\n                    return count;',
         '                    nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + ctx->sector_ofs);\n                    return count;'),
    ]
    for a, b in fixes:
        assert s.count(a) == 1, a
        s = s.replace(a, b)
    open(p, 'w').write(s)

# ... and extkeys.c: a key file line it can't read (a known key with a value of the wrong length, a title key listed
# twice) stopped it outright, printing the value. Emulators' key files often have such a line that nothing else
# minds: skip it instead, without the value. (The command line's keys stay strict.)
def patch_extkeys():
    p = src + "/extkeys.c"
    import re, sys
    s = open(p).read()
    s = s.replace('void parse_hex_key(unsigned char *key,',
        'static int ishex(char c);\n'
        'static int key_value_ok(const char *hex, unsigned int len) {\n'
        '    int ok = strlen(hex) == 2 * len;\n'
        '    for (unsigned int i = 0; ok && i < 2 * len; i++) ok = ishex(hex[i]);\n'
        '    if (!ok) fprintf(stderr, "[WARN] a key file value is not %"PRIu32" hex digits: skipped\\n", 2 * len);\n'
        '    return ok;\n'
        '}\n\n'
        'void parse_hex_key(unsigned char *key,', 1)
    tk = ('                parse_hex_key(rights_id, key, sizeof(rights_id));\n'
          '                parse_hex_key(titlekey, value, sizeof(titlekey));\n'
          '                settings_add_titlekey(settings, rights_id, titlekey);\n')
    assert s.count(tk) == 1, 'titlekeys'
    s = s.replace(tk, '                if (key_value_ok(key, sizeof(rights_id)) && key_value_ok(value, sizeof(titlekey))) {\n'
                      + tk.replace('                ', '                    ') + '                }\n')
    s, n = re.subn(r'(?m)^(\s*)parse_hex_key\(([^;]+), value, (sizeof\([^;]*\))\);',
                   r'\1if (key_value_ok(value, \3)) parse_hex_key(\2, value, \3);', s)
    assert n > 30, n
    dup = '        fprintf(stderr, " already has a corresponding titlekey!\\n");\n        exit(EXIT_FAILURE);\n'
    assert s.count(dup) == 1, 'duplicate'
    s = s.replace(dup, '        fprintf(stderr, " is listed twice: the first is used\\n");\n        return;\n')
    open(p, 'w').write(s)

patch_nca()
patch_extkeys()
