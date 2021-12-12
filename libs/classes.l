
;; - An instance is a dictionary, with a member called "__instanceof__"
;; - A class is a dictionary, with the following members:
;;   - parent
;;   - instance-methods
;;   - class-methods
;; - Eval of a list is as follows: (<receiver> <selector> <args...>)
;;   -  (funccall (at (at (at <recevier> '__instanceof__) 'instance-methods) <selector>) (cons <receiver> <args...>))


(def method-find (lambda (sel cls k)
		   (prog
		    (while cls
		      (setq m (at* (at cls k) sel))
		      (if m (return (cdr m)))
		      (setq cls (at cls 'parent))
		      )
		    )
		   )
     )


(def method-call (nlambda x
			  (setq sel (car (cdr x)))
			  (setq a (mapcar eval (cons (car x) (cdr (cdr x)))))
			  (setq r (car a))
			  (setq cls (at r '__instanceof__))
			  (setq m nil)
			  (if cls
			      (setq m (method-find sel cls 'instance-methods))
			    )
			  (if (and (not m) (or (not cls) (equal cls Metaclass)))
			      (setq m (method-find sel r 'class-methods))
			    )
			  (if (not m) (raise "classes.no-method" r sel))
			  (apply m a)
			  )
     )


(def eval (lambda* (x)
		   (if (atomp x) (enter (global) (eval x))
		     (apply method-call x)
		     )
		   )
     )


(def instance (lambda (cl)
		(dict (list (cons '__instanceof__ cl)))
		)
     )


(def class (lambda (parent)
	     (setq result (instance Metaclass))
	     (atput result 'parent parent)
	     (atput result 'instance-methods (dict))
	     (atput result 'class-methods (dict))
	     result
	     )
     )


(setq Metaclass (instance nil))
(atput Metaclass 'instance-methods (dict))
(atput Metaclass 'class-methods (dict))

(setq Object (class nil))

(atput Metaclass 'parent Object)

