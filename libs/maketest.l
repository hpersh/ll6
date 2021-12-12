(setq cflags (append
	      "-I . -Wall -Werror"
	      (if (enter os (getenv "DEBUG"))
		  " -g3"
		" -O2 -fomit-frame-pointer -DNDEBUG"
		)
	      )
      )

(target-add "math.so"
	    '("math.c" "ll.h")
	    '(and (run-shell-cmd (format "gcc -c -fPIC {} math.c -o math.o" cflags))
		  (run-shell-cmd "gcc -fPIC -shared math.o -o math.so -lm")
		  )
	    )

(target-add "%.so"
	    '("%.c" "ll.h")
	    '(and (run-shell-cmd (format "gcc -c -fPIC {} %.c -o %.o" cflags))
		  (run-shell-cmd "gcc -fPIC -shared %.o -o %.so")
		  )
	    )

(target-add "ll"
	    '("ll.c" "ll.h")
	    '(run-shell-cmd (format "gcc -rdynamic {} ll.c -lz -ldl -o ll" cflags))
	    )

(target-add "clean"
	    ()
	    '(run-shell-cmd "rm -fr ll *.o *.so")
	    )
(target-mark-phony "clean")

