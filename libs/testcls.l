(setq cl (load "classes.l"))
(setq Gtk-window (enter cl (class nil)))



(setq w (enter cl (instance Gtk-window)))
(atput w 'x 0)
(atput w 'y 0)
(atput w 'xsize 100)
(atput w 'ysize 50)
(print w)
(atput (at Gtk-window 'instance-methods) 'move '(lambda* (recvr x y) (atput recvr 'x x) (atput recvr 'y y)))
(enter cl (method-call w move 10 20))
(print w)
