;; Sieve of Eratosthenes

(def sqrt
     (lambda (c err)
       (setq x (div c 2))
       (while t
	 (setq e (abs (sub (mul x x) c)))
	 (if (lt e err) (break))
	 (setq x (div (add (mul x x) c) (mul 2.0 x)))
	 )
       x
       )
     )

(def sieve
     (lambda (n)
       (setq p (iota (sub n 1) 2))
       (filter (lambda* (d)
			(setq^ p (filter (lambda* (x)
						  (not (and (gt x d) (equal (mod x d) 0)))
						  )
					 p
					 )
			       )
			)
	       (iota (add (int (sqrt n)) 1) 2)
	       )
       p
       )
     )
