
(setq ser (load "./serialize.so"))

(setq d (dict '(("foo" . 123) ("bar" . 42))))

(print (format "{}\n" d))

(setq b (enter ser (write d)))

(print (format "{}\n" (repr b)))

(setq bb (enter ser (read b)))

(print (format "{}\n" (repr bb)))
