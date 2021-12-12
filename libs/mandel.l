
(def mandel-iter
     (lambda (z c)
       (add (mul z z) c)
       )
     )

(def mandel
     (lambda (z c n)
       (setq k 0)
       (while (lt k n)
	 (print (format "{}\t{}\t{}\n" k z (abs z)))
	 (setq z (mandel-iter z c))
	 (setq k (add k 1))
	 )
       )
     )
