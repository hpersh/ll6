

(setq gtk (load "gtk.l"))

(enter gtk (progn
	     (init ())
	     (setq w (enter cls (eval (Window new WINDOW_TOPLEVEL))))
	     (enter cls (eval (w show)))
	     (main)
	     )
       )
