#!/usr/bin/awk

BEGIN {
    active = 0;
}

/<\/test>/ {
    active = 0;
}

/^> / {
    if (active == 0)  next;
    printf "%s\n", substr($0, 3) >> "test.l";
}

/^< / {
    if (active == 0)  next;
    printf "%s\n", substr($0, 3) >> "golden.out";
}

/<test.*>/ {
    active = 1;
}
