

(def iota (lambda (x)
	    (setq result nil)
	    (setq i 0)
	    (while (lt i x)
	      (setq result (cons i result))
	      (setq i (add i 1))
	      )
	    (reverse result)
	    )
     )


(def cartesian (nlambda x
			(setq result nil)
			(setq y (mapcar pass x))
