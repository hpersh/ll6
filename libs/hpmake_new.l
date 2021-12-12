(setq os (load "./os.so"))
(setq re (load "./regexp.so"))

(setq recipes (dict))
(setq dependencies (dict))
(setq phonies (set))
(setq visited (set))
(setq depth 0)
(setq env (dict '(("log-level" . 1) ("halt-on-error" . t) ("optimize" . t))))


(def hookp
     (lambda (x)
       (and (equal (type x) "dptr")
	    (callablep (car x))
	    (equal (type (cdr x)) "dptr")
	    )
       )
     )


(def _mesg-print
     (lambda (mesg)
       (if (gt (at-default env "log-level" 1) 0)
	   (print mesg)
	 )
       )
     )


(def _log-mesg
     (lambda (log-lvl mesg)
       (if (ge (at-default env "log-level" 1) log-lvl)
	   (_mesg-print mesg)
	 )
       )
     )


(def run-shell-cmd
     (lambda (cmd)
       (_mesg-print (format "{}\n" cmd))
       (equal (enter os (system cmd)) 0)
       )
     )


(def dependency-add
     (lambda (target depends-on)
       (_log-mesg 2 (format "Adding dependency {} to target {}\n" depends-on target))
       (atput dependencies target (cons depends-on (at-default dependencies target ())))
       )
     )


(def target-add
     (lambda (target deps (rebuild-hook nil))
       (_log-mesg 2 (format "Adding target {}\n" target))
       (if rebuild-hook (atput recipes target rebuild-hook))
       (mapcar '(nlambda (x) (dependency-add target (car x)))
	       deps
	       )
       )
     )


(def target-mark-phony
     (lambda (target)
       (put phonies target)
       )
     )


(def phony-dependency-check
     (lambda (target dep)
       (_log-mesg 2 (format "Target {} is phony\n" target))
       (cons (_make dep) nil)
       )
     )


(def file-dependency-check
     (lambda (target target-time dep)
       (prog
	(_log-mesg 2 (format "Target {} depends on file {}\n" target dep))
	(if (or (not (at-default env "optimize" t))
		(not (at visited dep))
		)
	    (if (not (_make dep))
		(return '(() . ()))
	      )
	  (_log-mesg 2 (format "Dependency {} already visited, optimizing out" dep))
	  )
	(setq stat (enter os (stat dep)))
	(if (not stat)
	    (progn
	      (print (format "ERROR: Dependency {} of target {} does not exist\n" dep target) stderr)
	      (return '(() . ()))
	      )
	  )
	(if (and target-time (gt (cdr (at stat "st_mtime")) target-time))
	    (progn
	      (_log-mesg 2 (format "Dependency {} is newer than target\n" dep))
	      (return '(t . t))
	      )
	  )
	(_log-mesg 2 (format "Dependency {} is satisfied\n" dep))
	'(t . ())
	)
       )
     )


(def hook-dependency-check
     (lambda (target target-time dep)
       (_log-mesg 2 (format "Target {} depends on hook {}\n" target dep))
       (setq f (eval dep))
       (_log-mesg 2 (format "Hook returned {}\n" f))
       '(t . f)
       )
     )


(def dependency-check
     (lambda (target target-time dep)
       (cond ((equal (type dep) "str")
	      (if (at phonies dep)
		  (phony-dependency-check target target-time dep)
		(file-dependency-check target target-time dep)
		)
	      )
	     (t (hook-dependency-check target target-time dep))
	     )
       )
     )


(def rebuild-target
     (lambda (target)
       (prog
	(setq r (at recipes target))
	(if (not r)
	    (progn
	      (print (format "ERROR: No recipe for target {}\n" target) stderr)
	      (return nil)
	      )
	  )
	(setq h (cdr r))
	(if (not h) (return t))
	(_log-mesg 2 (format "Running recipe hook {}\n" h))
	(setq result (eval h))
	(_log-mesg 2 (format "Hook returned {}\n" result))
	result
	)
       )
     )


(def path-split
     (lambda (path)
       (setq a (enter re (match "(.*)/([^/]*)" path 3)))
       (if a (list (at a 1) (at a 2))
	 (list "." path)
	 )
       )
     )

     
(def match-files
     (lambda (dir pattern)
       (setq d (enter os (opendir dir)))
       (setq result ())
       (while (setq e (enter os (readdir d)))
	 (setq nm (cdr (at e "d_name")))
	 (if (enter re (match pattern nm))
	     (setq result (cons (append dir "/" nm) result))
	   )
	 )
       result
       )
     )


(def _make
     (lambda (target deps)
       (_log-mesg 2 (format "Making target {}\n" target))
       (setq depth (add depth 1))
       (put visited target)
       (setq rebuildf nil)
       (setq target-time nil)
       (setq phonyf (at phonies target))
       (cond (phonyf
	      (_log-mesg 2 (format "Target {} is phony\n" target))
	      (setq rebuild-flat t)
	      )
	     ((setq stat (enter os (stat target)))
	      (_log-mesg 2 (format "Target {} exists\n" target))
	      (setq target-time (cdr (at stat "st_mtime")))
	      )
	     (t
	      (_log-mesg 2 (format "Target {} does not exist\n" target))
	      (setq rebuildf t)
	      )
	     )
       (setq result t)
       (while deps
	 (setq dep (car deps))
	 (setq f (dependency-check target target-time dep))

	 (if (and (not (car f)) (at-default env "halt-on-error" t))
	     (progn
	       (setq result nil)
	       (setq rebuildf nil)
	       (break)
	       )
	   )
	 (setq rebuildf (or rebuildf (cdr f)))
	 (setq deps (cdr deps))
	 )

       (if rebuildf
	   (setq result (rebuild-target target))
	 )
       (setq depth (sub depth 1))
       result
       )
     )


(def _make
     (lambda (target)
       (_log-mesg 2 (format "Making target {}\n" target))
       (setq depth (add depth 1))
       (put visited target)
       (setq rebuildf nil)
       (setq target-time nil)
       (setq phonyf (at phonies target))
       (cond (phonyf
	      (_log-mesg 2 (format "Target {} is phony\n" target))
	      (setq rebuild-flat t)
	      )
	     ((setq stat (enter os (stat target)))
	      (_log-mesg 2 (format "Target {} exists\n" target))
	      (setq target-time (cdr (at stat "st_mtime")))
	      )
	     (t
	      (_log-mesg 2 (format "Target {} does not exist\n" target))
	      (setq rebuildf t)
	      )
	     )
       (setq result t)
       (setq deps (cdr (at dependencies target)))
       (while deps
	 (setq dep (car deps))
	 (setq f (dependency-check target target-time dep))

	 (if (and (not (car f)) (at-default env "halt-on-error" t))
	     (progn
	       (setq result nil)
	       (setq rebuildf nil)
	       (break)
	       )
	   )
	 (setq rebuildf (or rebuildf (cdr f)))
	 (setq deps (cdr deps))
	 )

       (if rebuildf
	   (setq result (rebuild-target target))
	 )
       (setq depth (sub depth 1))
       result
       )
     )


(def make
     (lambda (target)
       (setq visited (set))
       (setq depth 0)
       (_make target (cdr (at dependencies target)))
       )
     )

;; Mainline

(setq progname (car argv))
(setq argv (cdr argv))

(def usage
     (lambda ()
       (print (format "usage: {} [-k] [-v <verbosity>] <makefile> [targets ...]\n" progname) stderr)
       (exit 1)
       )
     )


(while argv
  (setq arg (car argv))
  (if (not (and (ge (size arg) 2)
		(equal (at arg 0) "-")
		)
	   )
      (break)
    )
  (setq opt (at arg 1))
  (cond ((equal opt "k")
	 (atput env "halt-on-error" nil)
	 )
	((equal opt "v")
	 (setq argv (cdr argv))
	 (if (not argv) (usage))
	 (atput env "log-level" (int (car argv)))
	 )
	(t (usage))
	)
  (setq argv (cdr argv))
  )

(if (not argv) (usage))

(read (open (car argv) "r"))

(mapcar '(nlambda (x) (make (car x)))
	(cdr argv)
	)
