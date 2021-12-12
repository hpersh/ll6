;; This is a comment

;; Test read integers
123
-456

;; Test read floats
3.14
-4.56

;; Test read complex
1+2i
-3-4i

;; Test read strings
"foo"
"abc\ndef"
"Included \" included"

;; Test quoting
;; Test read symbols
t
nil
'abc

;; Test quoting
;; Test read lists
()
'(1 2 3)
'(1 . 2)
'(1 2 3 . 4)
'(1 2 (3 4) 5 6)

;; Test naked callable forms
(lambda)
(lambda* "This can be anything")
(nlambda This too)
(macro 1 2 3 4)

;; Test environment binding
(setq a 123)
a
(let () (setq^ b 456))
b
(def c (1 2 3))
c
(unsetq c)
(try
 c
 (lambda* (e)
	  (assert (and (equal (car e) "system.not-bound")
		       (equal (cadr e) '(c))
		       )
		  )
	  )
 (assert nil)
)



;; Test common operations
(assert (eq 'a 'a))

;; Test integers
(assert (equal (add 1 2) 3))
(assert (equal (sub 1 2) -1))
(assert (equal (mul 3 4) 12))
(assert (equal (div 15 3) 5))
(assert (equal (mod 13 4) 1))

;; Test floats

;; Test complex
(assert (equal (add 1+2i 3+4i) 4+6i))
(assert (equal (abs 3+4i) 5.0))

;; Test symbols

;; Test strings

(setq s "The rain in Spain")
(assert (equal (size s) 17))
(assert (equal s "The rain in Spain"))
(assert (equal (at s 0) "T"))
(assert (equal (at s -1) "n"))

(try
 (atput s 0 "x")
 (lambda* (e)
	  (assert (equal (car e) "system.bad-argument"))
	  )
 (assert nil)
 )
(assert (equal (append "abc" "def") "abcdef"))

;; (def if
;;      (macro (x)
;; 	    (list 'cond
;; 		  (list (car x) (car (cdr x)))
;; 		  (list t (cond ((equal (size x) 3) (car (cdr (cdr x)))) (t nil)))
;; 		  )
;; 	    )
;;      )

;; (def while
;;      (macro (x)
;;             (list 'prog
;;                   'loop
;;                   (list 'cond
;;                         (cons (car x) (append (cdr x) '((goto loop))))
;;                         '(t nil)
;;                         )
;;                   )
;;             )
;;      )

;; (setq i 0)
;; (while (lt i 10) (print i) (setq i (add i 1)))


;; (def fibber
;;      (lambda ()
;;        (closure
;; 	'(prog
;; 	  (setq r (add a b))
;; 	  (setq b a)
;; 	  (setq a r)
;; 	  r
;; 	  )
;; 	(dict '((a . 0) (b . 1)))
;; 	)
;;        )
;;      )

;; (setq f (fibber))

;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")
;; (print (eval f))
;; (print "\n")

;; <doc>
;; ## Function: foo
;; ### Type
;; subr
;; ### Form
;; (foo _sexpr_)
;; ### Description
;; Foo the given _sexpr_
;; ### Return value
;; Same as first argument
;; ### Exceptions
;; None
;; ### See also
;; ### Examples
;; > -> (quote foo)  
;; > foo
;; </doc>
