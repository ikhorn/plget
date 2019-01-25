#!/bin/sh

tfile=$(mktemp /tmp/test_libbpf_XXXXXXXX.c)
ofile=${tfile%.c}.o

cat > $tfile <<EOL
#include <libbpf.h>

int main(void)
{
	struct bpf_object *obj;
	return !!bpf_object__find_map_by_name(obj, "some_map");
}
EOL

gcc $tfile -o $ofile >/dev/null 2>&1
if [ $? -ne 0 ]; then echo "FAIL"; fi
/bin/rm -f $tfile $ofile
