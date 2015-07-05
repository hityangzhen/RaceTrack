#ifndef __RACE_PROFILER_H
#define __RACE_PROFILER_H

#include "core/basictypes.h"
#include "core/execution_control.hpp"
#include "race/race.h"
#include "race/djit.h"
#include "race/eraser.h"
#include "race/race_track.h"
#include "race/helgrind.h"
#include "race/thread_sanitizer.h"
#include "race/fast_track.h"
#include "race/loft.h"
#include "race/acculock.h"
#include "race/multilock_hb.h"
#include "race/simple_lock.h"
#include "race/simplelock_plus.h"

namespace race{

class Profiler:public ExecutionControl {
public:
	Profiler():race_db_(NULL),
			race_rp_(NULL),
			djit_analyzer_(NULL),
			eraser_analyzer_(NULL),
			race_track_analyzer_(NULL),
			helgrind_analyzer_(NULL),
			thread_sanitizer_analyzer_(NULL),
			fast_track_analyzer_(NULL),
			loft_analyzer_(NULL),
			acculock_analyzer_(NULL),
			multilock_hb_analyzer_(NULL),
			simple_lock_analyzer_(NULL),
			simplelock_plus_analyzer_(NULL)

	{}
	~Profiler() {}

protected:
	void HandlePreSetup();
	void HandlePostSetup();
	bool HandleIgnoreMemAccess(IMG img);
	void HandleProgramExit();

	RaceDB *race_db_;
	RaceReport *race_rp_;
	Djit *djit_analyzer_;
	Eraser *eraser_analyzer_;
	RaceTrack *race_track_analyzer_;
	Helgrind *helgrind_analyzer_;
	ThreadSanitizer *thread_sanitizer_analyzer_;
	FastTrack *fast_track_analyzer_;
	Loft *loft_analyzer_;
	AccuLock *acculock_analyzer_;
	MultiLockHb *multilock_hb_analyzer_;
	SimpleLock *simple_lock_analyzer_;
	SimpleLockPlus *simplelock_plus_analyzer_;

private:
	DISALLOW_COPY_CONSTRUCTORS(Profiler);
};

} //namespace race

#endif