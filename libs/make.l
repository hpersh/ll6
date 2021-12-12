(setq hpm (new Module "../libs/hpmake.l"))

(setq cflags "-g")
(setq libs '("os.so" "math.so" "datetime.so" "socket.so" "regexp.so"))
(setq lib-opts (new Dict '(("math.so" . "-lm"))))

(setq targets (new Dict))

(foreach (lambda (target)
	   (setq basename (hpm.suffix-remove target))
	   (setq src (format "../src/{0}.c" basename))
	   (atput targets target (new hpm.Target target target (concat List src  "../src/ll.h")
	                              (format "gcc -c -fPIC {0} {1} -o {2}.o; gcc -fPIC -shared {2}.o -o {2}.so {3}; rm -f {2}.o"
				              cflags src basename (at-default lib-opts target "")
					      )
				      )
		  )
	   )
	 libs
	 )

(atput targets "all"   (new hpm.Target "all"   nil (mapcar (lambda (x) (cdr x)) (new List targets)) nil))
(atput targets "clean" (new hpm.Target "clean" nil nil "rm *.so"))

;; (print targets)

(enter hpm (run targets))
