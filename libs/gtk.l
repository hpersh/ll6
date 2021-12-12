(setq cls (load "classes.l"))

(def method-call
     (nlambda x
	      (enter cls (method-call x))
	      )
     )


(setq gtk (load "./gtk.so"))

(setq WINDOW_TOPLEVEL (enter gtk WINDOW_TOPLEVEL))

;; Class: Window

(def init
     (lambda (args)
       (enter gtk (init args))
       )
     )

(setq Window (enter cls (class nil)))

(atput (at Window 'class-methods) 'new
       '(lambda* (cls f)
		 (setq inst (enter cls (instance Window)))
		 (atput inst 'ptr (enter gtk (window-new f)))
		 inst
		 )
       )

(atput (at Window 'instance-methods) 'show
       '(lambda* (w)
		 (enter gtk (widget-show (at w 'ptr)))
		 )
       )


(def main
     (lambda ()
       (enter gtk (main))
       )
     )





