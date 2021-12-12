#!/usr/bin/awk

BEGIN {
    state = "idle";
    n = 0;
}

/<\/doc>/ {
    a[n] = s;
    ++n;
    state = "idle";
}

{
    if (state != "idle") {
        if (s != "") {
            s = s "\n";
        }
        sub("^;*\\s*", "");        
        s = s $0;
    }
}

/<doc>/ {
    state = "doc";
    s = "";
}

function key_get(s)
{
    match(s, "^## [^\n]*\n");
    if (RLENGTH < 0) {
        print "ERROR" > "/dev/stderr";
        print s > "/dev/stderr";
        exit 1;
    }
    return (substr(s, 3, RLENGTH - 1 - 3));
}

END {
    if (n == 0) {
        exit;
    }
    k = n - 1;
    f = 1;
    while (f != 0) {
        f = 0;
        for (i = 0; i < k; ++i) {
            if (key_get(a[i]) <= key_get(a[i + 1]))  continue;
            temp = a[i];
            a[i] = a[i + 1];
            a[i + 1] = temp;
            f = 1;
        }
    }
    for (i = 0; i < n; ++i) {
	if (i > 0) {
	    print "---";
	}
        print a[i];
    }
}
