
#include <unistd.h>
#include "race/verifier.h"
#include "core/log.h"
#include "core/pin_util.h"

namespace race
{

Verifier::Verifier():internal_lock_(NULL),verify_lock_(NULL),prace_db_(NULL),
	filter_(NULL),unit_size_(4)
	{}

Verifier::~Verifier() 
{
	delete internal_lock_;
	delete verify_lock_;
	delete filter_;

	for(ThreadVectorClockMap::iterator iter=thd_vc_map_.begin();
		iter!=thd_vc_map_.end();iter++) {
		delete iter->second;
	}
	//
	for(PStmtMetasMap::iterator iter=pstmt_metas_map_.begin();
		iter!=pstmt_metas_map_.end();iter++) {
		if(iter->second)
			delete iter->second;
	}
	for(ThreadMetasMap::iterator iter=thd_metas_map_.begin();
		iter!=thd_metas_map_.end();iter++) {
		if(iter->second)
			delete iter->second;
	}

	//
	for(MutexMeta::Table::iterator iter=mutex_meta_table_.begin();
		iter!=mutex_meta_table_.end();iter++) {
		if(iter->second)
			ProcessFree(iter->second);
	}

	for(RwlockMeta::Table::iterator iter=rwlock_meta_table_.begin();
		iter!=rwlock_meta_table_.end();iter++) {
		if(iter->second)
			ProcessFree(iter->second);
	}
}

bool Verifier::Enabled()
{
	return knob_->ValueBool("race_verify");
}

void Verifier::Register()
{
	knob_->RegisterBool("race_verify","whether enable the race verify","0");
	knob_->RegisterInt("unit_size_","the mornitoring granularity in bytes","4");
}

void Verifier::Setup(Mutex *internal_lock,Mutex *verify_lock,PRaceDB *prace_db)
{
	internal_lock_=internal_lock;
	verify_lock_=verify_lock;
	prace_db_=prace_db;
	unit_size_=knob_->ValueInt("unit_size_");
	filter_=new RegionFilter(internal_lock_->Clone());

	desc_.SetHookBeforeMem();
	desc_.SetHookPthreadFunc();
	desc_.SetHookMallocFunc();
	desc_.SetHookAtomicInst();
}

void Verifier::ImageLoad(Image *image,address_t low_addr,address_t high_addr,
	address_t data_start,size_t data_size,address_t bss_start,size_t bss_size)
{
	DEBUG_ASSERT(low_addr && high_addr && high_addr>low_addr);
	if(data_start) {
		DEBUG_ASSERT(data_size);
		AllocAddrRegion(data_start,data_size);
	}
	if(bss_start) {
		DEBUG_ASSERT(bss_size);
		AllocAddrRegion(bss_start,bss_size);
	}
}

void Verifier::ImageUnload(Image *image,address_t low_addr,address_t high_addr,
	address_t data_start,size_t data_size,address_t bss_start,size_t bss_size)
{
	DEBUG_ASSERT(low_addr);
	if(data_start)
		FreeAddrRegion(data_start);
	if(bss_start)
		FreeAddrRegion(bss_start);
}

void Verifier::ThreadStart(thread_t curr_thd_id,thread_t parent_thd_id)
{
	// PinSemaphore *pin_sema=new PinSemaphore;
	SysSemaphore *sys_sema=new SysSemaphore(0);
	VectorClock *curr_vc=new VectorClock;
	ScopedLock lock(internal_lock_);
	//thread vector clock
	curr_vc->Increment(curr_thd_id);
	//not the main thread
	if(parent_thd_id!=INVALID_THD_ID) {
		VectorClock *parent_vc=thd_vc_map_[parent_thd_id];
		DEBUG_ASSERT(parent_vc);
		curr_vc->Join(parent_vc);
	}
	thd_vc_map_[curr_thd_id]=curr_vc;
	//thread semaphore
	if(thd_smp_map_.find(curr_thd_id)==thd_smp_map_.end()) {
		// thd_smp_map_[curr_thd_id]=pin_sema;
		// thd_smp_map_[curr_thd_id]->Init();
		thd_smp_map_[curr_thd_id]=sys_sema;
	}
	//all threads are available at the beginning
	avail_thd_set_.insert(curr_thd_id);
}

void Verifier::ThreadExit(thread_t curr_thd_id,timestamp_t curr_thd_clk)
{
INFO_FMT_PRINT("=============postpone set size:[%ld]==============\n",pp_thd_set_.size());
	ScopedLock lock(internal_lock_);

	//clear current thread accessed metas
	// if(thd_metas_map_.find(curr_thd_id)!=thd_metas_map_.end() && 
	// 	thd_metas_map_[curr_thd_id]!=NULL)
	// 	delete thd_metas_map_[curr_thd_id];
	// thd_metas_map_.erase(curr_thd_id);

	//free the semaphore
	delete thd_smp_map_[curr_thd_id];
	thd_smp_map_.erase(curr_thd_id);
	//
	avail_thd_set_.erase(curr_thd_id);
	//must remove from postpone thread set to make current thread exit
	if(pp_thd_set_.find(curr_thd_id)!=pp_thd_set_.end())
		pp_thd_set_.erase(curr_thd_id);
	if(avail_thd_set_.empty())
		ChooseRandomThreadAfterAllUnavailable();
}

void Verifier::BeforePthreadJoin(thread_t curr_thd_id,timestamp_t curr_thd_clk,
	Inst *inst,thread_t child_thd_id)
{
	ScopedLock lock(internal_lock_);
	BlockThread(curr_thd_id);
	//current thread is blocked
	if(avail_thd_set_.empty()) 
		ChooseRandomThreadAfterAllUnavailable();
	INFO_FMT_PRINT("================before pthread join avail_thd_set_.size:[%ld]\n",avail_thd_set_.size());
}

void Verifier::AfterPthreadJoin(thread_t curr_thd_id,timestamp_t curr_thd_clk,
	Inst *inst,thread_t child_thd_id)
{
	ScopedLock lock(internal_lock_);
	//thread vector clock
	VectorClock *curr_vc=thd_vc_map_[curr_thd_id];
	VectorClock *child_vc=thd_vc_map_[child_thd_id];
	curr_vc->Join(child_vc);
	curr_vc->Increment(curr_thd_id);

	UnblockThread(curr_thd_id);
}

void Verifier::AfterPthreadCreate(thread_t curr_thd_id,timestamp_t curr_thd_clk,
  	Inst *inst,thread_t child_thd_id)
{
	ScopedLock lock(internal_lock_);
	thd_vc_map_[curr_thd_id]->Increment(curr_thd_id);
}

void Verifier::AfterMalloc(thread_t curr_thd_id, timestamp_t curr_thd_clk,
    Inst *inst, size_t size, address_t addr)
{
	AllocAddrRegion(addr,size);
}

void Verifier::AfterCalloc(thread_t curr_thd_id, timestamp_t curr_thd_clk,
    Inst *inst, size_t nmemb, size_t size,address_t addr)
{
	AllocAddrRegion(addr,size *nmemb);
}

void Verifier::BeforeRealloc(thread_t curr_thd_id, timestamp_t curr_thd_clk,
    Inst *inst, address_t ori_addr, size_t size)
{
	FreeAddrRegion(ori_addr);
}
  
void Verifier::AfterRealloc(thread_t curr_thd_id, timestamp_t curr_thd_clk,
    Inst *inst, address_t ori_addr, size_t size,address_t new_addr)
{
	AllocAddrRegion(new_addr,size);
}

void Verifier::BeforeFree(thread_t curr_thd_id, timestamp_t curr_thd_clk,
    Inst *inst, address_t addr)
{
	FreeAddrRegion(addr);
}

void Verifier::BeforeMemRead(thread_t curr_thd_id,timestamp_t curr_thd_clk,
	Inst *inst,address_t addr,size_t size)
{
	ChooseRandomThreadBeforeExecute(addr,curr_thd_id);
	ProcessReadOrWrite(curr_thd_id,inst,addr,size,RACE_EVENT_READ);
}

void Verifier::BeforeMemWrite(thread_t curr_thd_id,timestamp_t curr_thd_clk,
	Inst *inst,address_t addr,size_t size)
{
	ChooseRandomThreadBeforeExecute(addr,curr_thd_id);
	ProcessReadOrWrite(curr_thd_id,inst,addr,size,RACE_EVENT_WRITE);
}

void Verifier::BeforePthreadMutexLock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	// MAP_KEY_NOTFOUND_NEW(mutex_meta_table_,addr,MutexMeta);
	// MutexMeta *mutex_meta=mutex_meta_table_[addr];
	MutexMeta *mutex_meta=GetMutexMeta(addr);

	thread_t thd_id=mutex_meta->GetOwner();
INFO_FMT_PRINT("================mutex lock owner:[%lx]\n",
		thd_id);
	//other thread has acquire the lock
	BlockThread(curr_thd_id);
	if(thd_id!=0) {	
		if(avail_thd_set_.empty() && 
			pp_thd_set_.find(thd_id)!=pp_thd_set_.end()) {
			WakeUpPostponeThread(thd_id);
		}
		// else do nothing blocked
	}
	INFO_FMT_PRINT("================before thread mutex lock avail_thd_set_ size:[%ld]\n",
		avail_thd_set_.size());
}
	
void Verifier::AfterPthreadMutexLock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	//set the owner
	ScopedLock lock(internal_lock_);
	// MutexMeta *mutex_meta=mutex_meta_table_[addr];
	MutexMeta *mutex_meta=GetMutexMeta(addr);
	//set the vector clock
	VectorClock *curr_vc=thd_vc_map_[curr_thd_id];
	curr_vc->Join(&mutex_meta->vc);

	mutex_meta->SetOwner(curr_thd_id);
	//if current not blocked, blk_thd_set_ and avail_thd_set_
	//result will not be changed
	UnblockThread(curr_thd_id);
}

void Verifier::BeforePthreadMutexUnlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	MutexMeta *mutex_meta=GetMutexMeta(addr);
	//set the vector clock
	VectorClock *curr_vc=thd_vc_map_[curr_thd_id];
	mutex_meta->vc=*curr_vc;
	curr_vc->Increment(curr_thd_id);
}

void Verifier::AfterPthreadMutexUnlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	//clear the owner
	ScopedLock lock(internal_lock_);
	// MutexMeta *mutex_meta=mutex_meta_table_[addr];
	MutexMeta *mutex_meta=GetMutexMeta(addr);
	mutex_meta->SetOwner(0);
}
// untested
void Verifier::BeforePthreadMutexTryLock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	BeforePthreadMutexLock(curr_thd_id,curr_thd_clk,inst,addr);
}
// untested
void Verifier::AfterPthreadMutexTryLock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr,int ret_val)
{
	if(ret_val==0)
		AfterPthreadMutexLock(curr_thd_id,curr_thd_clk,inst,addr);
}

void Verifier::BeforePthreadRwlockRdlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	RwlockMeta *rwlock_meta=GetRwlockMeta(addr);
	thread_t wrlock_thd_id=rwlock_meta->GetWrlockOwner();
	BlockThread(curr_thd_id);

	if(wrlock_thd_id!=0) {
		if(avail_thd_set_.empty() && 
			pp_thd_set_.find(wrlock_thd_id)!=pp_thd_set_.end()) {
			WakeUpPostponeThread(wrlock_thd_id);
		}
	}
}

void Verifier::AfterPthreadRwlockRdlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	RwlockMeta *rwlock_meta=GetRwlockMeta(addr);
	//set the vector clock
	VectorClock *curr_vc=thd_vc_map_[curr_thd_id];
	curr_vc->Join(&rwlock_meta->vc);

	UnblockThread(curr_thd_id);
	rwlock_meta->AddRdlockOwner(curr_thd_id);
	rwlock_meta->ref++;
}

void Verifier::BeforePthreadRwlockWrlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	RwlockMeta *rwlock_meta=GetRwlockMeta(addr);

	RwlockMeta::ThreadSet *rdlock_thd_set=rwlock_meta->GetRdlockOwners();
	BlockThread(curr_thd_id);
	//here we only consider the simple scene
	//there are no other 
	if(rwlock_meta->HasRdlockOwner()) {
		if(avail_thd_set_.empty()) {
			//we wakeup all readers
			for(RwlockMeta::ThreadSet::iterator iter=rdlock_thd_set->begin();
				iter!=rdlock_thd_set->end();iter++) {
				WakeUpPostponeThread(*iter);
			}
		}
	}
}

void Verifier::AfterPthreadRwlockWrlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	UnblockThread(curr_thd_id);
	RwlockMeta *rwlock_meta=GetRwlockMeta(addr);
	//set the vector clock
	VectorClock *curr_vc=thd_vc_map_[curr_thd_id];
	curr_vc->Join(&rwlock_meta->vc);

	rwlock_meta->SetWrlockOwner(curr_thd_id);
	rwlock_meta->ref++;
}

void Verifier::BeforePthreadRwlockUnlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	RwlockMeta *rwlock_meta=GetRwlockMeta(addr);
	VectorClock *curr_vc=thd_vc_map_[curr_thd_id];
	rwlock_meta->ref--;
	rwlock_meta->wait_vc.Join(curr_vc);

	if(rwlock_meta->ref==0) {
		rwlock_meta->vc=rwlock_meta->wait_vc;
		rwlock_meta->wait_vc.Clear();
	}

	curr_vc->Increment(curr_thd_id);
}

void Verifier::AfterPthreadRwlockUnlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	ScopedLock lock(internal_lock_);
	RwlockMeta *rwlock_meta=GetRwlockMeta(addr);
	//we do not know rdlock or wrlock
	rwlock_meta->SetWrlockOwner(0);
	rwlock_meta->RemoveRdlockOwner(curr_thd_id);
}

void Verifier::BeforePthreadRwlockTryRdlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	BeforePthreadRwlockRdlock(curr_thd_id,curr_thd_clk,inst,addr);
}

void Verifier::AfterPthreadRwlockTryRdlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr,int ret_val)
{
	if(ret_val==0)
		AfterPthreadRwlockRdlock(curr_thd_id,curr_thd_clk,inst,addr);
}

void Verifier::BeforePthreadRwlockTryWrlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr)
{
	BeforePthreadRwlockWrlock(curr_thd_id,curr_thd_clk,inst,addr);
}

void Verifier::AfterPthreadRwlockTryWrlock(thread_t curr_thd_id,
	timestamp_t curr_thd_clk,Inst *inst,address_t addr,int ret_val)
{
	if(ret_val==0)
		AfterPthreadRwlockWrlock(curr_thd_id,curr_thd_clk,inst,addr);
}

/**
 * multi-threaded region
 */
void Verifier::ChooseRandomThreadBeforeExecute(address_t addr,thread_t curr_thd_id)
{
	if(FilterAccess(addr))	
		return ;
	VerifyLock();
	//generate random thread id
	if(!avail_thd_set_.empty()) {
		while(RandomThread(avail_thd_set_)!=curr_thd_id) {
			VerifyUnlock();
			Sleep(1000);
		}
	}
}

/**
 * single-threaded region,when entering this function, current thread
 * has hold the verify_lock_
 */
void Verifier::ProcessReadOrWrite(thread_t curr_thd_id,Inst *inst,address_t addr,
	size_t size,RaceEventType type)
{
INFO_FMT_PRINT("========process read or write,curr_thd_id:[%lx]=======\n",curr_thd_id);
	//get the potential statement
	std::string file_name=inst->GetFileName();
	size_t found=file_name.find_last_of("/");
	file_name=file_name.substr(found+1);
	int line=inst->GetLine();
	PStmt *pstmt=prace_db_->GetPStmt(file_name,line);
	DEBUG_ASSERT(pstmt);

	address_t start_addr=UNIT_DOWN_ALIGN(addr,unit_size_);
	address_t end_addr=UNIT_UP_ALIGN(addr+size,unit_size_);

	//first pstmt set of the pstmt pairs
	PStmtSet first_pstmts;
	//traverse the handled potential statements
	for(PStmtMetasMap::iterator iter=pstmt_metas_map_.begin();
		iter!=pstmt_metas_map_.end();iter++) {
		//find all matched potential statements
		if(prace_db_->SecondPotentialStatement(iter->first,pstmt)) {
			first_pstmts.insert(iter->first);
		}
	}

	//first accessed stmt of the potentail stmt pair,needed to be postponed 
	if(first_pstmts.empty()) {
		//add metas to pstmt accessed metas
		
		//accumulate the metas into pstmt corresponding metas
		MAP_KEY_NOTFOUND_NEW(pstmt_metas_map_,pstmt,MetaSet);
		MAP_KEY_NOTFOUND_NEW(thd_metas_map_,curr_thd_id,MetaSet);

		for(address_t iaddr=start_addr;iaddr<end_addr;iaddr+=unit_size_) {
			Meta *meta=GetMeta(iaddr);
			DEBUG_ASSERT(meta);
			//add snapshot
			timestamp_t curr_thd_clk=thd_vc_map_[curr_thd_id]->GetClock(curr_thd_id);
			meta->AddMetaSnapshot(curr_thd_id,MetaSnapshot(curr_thd_clk,type,inst));

			pstmt_metas_map_[pstmt]->insert(meta);
			thd_metas_map_[curr_thd_id]->insert(meta);

INFO_FMT_PRINT("===========first pstmt:[%lx]===========\n",curr_thd_id);
		}
		//postpone current thread
		PostponeThread(curr_thd_id);
	} else {
		//accumulate postponed threads
		PostponeThreadSet pp_thds;
		for(PStmtSet::iterator iter=first_pstmts.begin();iter!=first_pstmts.end();
			iter++) {
			PStmt *first_pstmt=*iter;
			RacedMeta(first_pstmt,start_addr,end_addr,pstmt,inst,curr_thd_id,
				type,pp_thds);
		}

		if(!pp_thds.empty()) {
			// //clear current thread accessed metas and pstmt corresponding metas
			// MetaSet *metas=thd_metas_map_[curr_thd_id];
			// DEBUG_ASSERT(metas);
			// ClearPStmtCorrespondingMetas(pstmt,metas);
			HandleRace(&pp_thds,curr_thd_id);
		// have no shared memory
		}else
			HandleNoRace(curr_thd_id);
	}
INFO_FMT_PRINT("=========process read or write end:[%lx]=========\n",curr_thd_id);
}

void Verifier::FreeAddrRegion(address_t addr)
{
	ScopedLock lock(internal_lock_);
	if(!addr) return ;
	size_t size=filter_->RemoveRegion(addr,false);

	address_t start_addr=UNIT_DOWN_ALIGN(addr,unit_size_);
	address_t end_addr=UNIT_UP_ALIGN(addr+size,unit_size_);

	//for memory
	for(address_t iaddr=start_addr;iaddr<end_addr;iaddr +=unit_size_) {
		Meta::Table::iterator it=meta_table_.find(iaddr);
		if(it!=meta_table_.end()) {
			ProcessFree(it->second);
			meta_table_.erase(it);
		}
	}
}

void Verifier::AllocAddrRegion(address_t addr,size_t size)
{
	ScopedLock lock(internal_lock_);
	DEBUG_ASSERT(addr && size);
	filter_->AddRegion(addr,size,false);
}

Verifier::Meta* Verifier::GetMeta(address_t iaddr)
{
	Meta::Table::iterator it=meta_table_.find(iaddr);
	if(it==meta_table_.end()) {
		Meta *meta=new Meta(iaddr);
		meta_table_[iaddr]=meta;
		return meta;
	}
	return it->second;
}

Verifier::MutexMeta* Verifier::GetMutexMeta(address_t iaddr)
{
	MutexMeta::Table::iterator it=mutex_meta_table_.find(iaddr);
	if(it==mutex_meta_table_.end()) {
		MutexMeta *mutex_meta=new MutexMeta();
		mutex_meta_table_[iaddr]=mutex_meta;
		return mutex_meta;
	}
	return it->second;
}

Verifier::RwlockMeta* Verifier::GetRwlockMeta(address_t iaddr)
{
	RwlockMeta::Table::iterator it=rwlock_meta_table_.find(iaddr);
	if(it==rwlock_meta_table_.end()) {
		RwlockMeta *rwlock_meta=new RwlockMeta();
		rwlock_meta_table_[iaddr]=rwlock_meta;
		return rwlock_meta;
	}
	return it->second;
}

void Verifier::ProcessFree(Meta *meta)
{
	delete meta;
}

void Verifier::ProcessFree(MutexMeta *mutex_meta)
{
	delete mutex_meta;
}

void Verifier::ProcessFree(RwlockMeta *rwlock_meta)
{
	delete rwlock_meta;
}

//
void Verifier::HandleNoRace(thread_t curr_thd_id)
{
INFO_PRINT("=================handle no race===================\n");
	PostponeThread(curr_thd_id);
}
//
void Verifier::HandleRace(PostponeThreadSet *pp_thds,thread_t curr_thd_id)
{
INFO_PRINT("=================handle race===================\n");
	//clear raced info whenever
	for(PostponeThreadSet::iterator iter=pp_thds->begin();iter!=pp_thds->end();
		iter++) {
INFO_FMT_PRINT("+++++++++++++++++++pp_thd_id:[%lx]+++++++++++++++++\n",*iter);
		//clear raced metas
		// delete thd_metas_map_[*iter];
		// thd_metas_map_[*iter]=NULL;
	}

	// delete thd_metas_map_[curr_thd_id];
	// thd_metas_map_[curr_thd_id]=NULL;

	//wake up postpone thread set,postpone current thread
	if(!RandomBool()) {
		WakeUpPostponeThreadSet(pp_thds);
		PostponeThread(curr_thd_id);
	}
	//must free the lock if current thread execute continuously
	else
		VerifyUnlock();
}

/**
 * must free lock at each branch of the procedure
 */
void Verifier::PostponeThread(thread_t curr_thd_id)
{
INFO_FMT_PRINT("=================postpone thread:[%lx]===================\n",curr_thd_id);

	//correspond to before_mutex_lock
	InternalLock();
INFO_FMT_PRINT("=================avail_thd_set_ size:[%ld]===================\n",avail_thd_set_.size());
	//current thread is the only available thread, others may be blocked by some sync
	//operations, we should not postpone it

	if(avail_thd_set_.size()==1 && pp_thd_set_.empty()) {
		InternalUnlock();
		VerifyUnlock();		
		return ;
	}

	pp_thd_set_.insert(curr_thd_id);
	avail_thd_set_.erase(curr_thd_id);
	//must choose one thread execute continuously if all threads are unavailable
	if(avail_thd_set_.empty())
		ChooseRandomThreadAfterAllUnavailable();

	InternalUnlock();
	VerifyUnlock();
	//semahore wait
	thd_smp_map_[curr_thd_id]->Wait();
INFO_FMT_PRINT("=================after wait:[%lx]===================\n",curr_thd_id);
}

void Verifier::ChooseRandomThreadAfterAllUnavailable()
{
	if(pp_thd_set_.empty())
		return ;
	thread_t thd_id=RandomThread(pp_thd_set_);
INFO_FMT_PRINT("=================needed to wakeup:[%lx]===================\n",thd_id);
	DEBUG_ASSERT(thd_smp_map_[thd_id]);
	// if(thd_smp_map_[thd_id]->IsWaiting())
	// 	thd_smp_map_[thd_id]->Post();
	WakeUpPostponeThread(thd_id);
}

inline void Verifier::WakeUpPostponeThread(thread_t thd_id)
{
	thd_smp_map_[thd_id]->Post();
	pp_thd_set_.erase(thd_id);
	avail_thd_set_.insert(thd_id);
}

void Verifier::WakeUpPostponeThreadSet(PostponeThreadSet *pp_thds)
{
INFO_FMT_PRINT("=================wakeup pp_thds size:[%ld]===================\n",pp_thds->size());
	DEBUG_ASSERT(pp_thds);
	for(PostponeThreadSet::iterator iter=pp_thds->begin();iter!=pp_thds->end();
		iter++) {
		// if(thd_smp_map_[*iter]->IsWaiting())
		// 	thd_smp_map_[*iter]->Post();
		WakeUpPostponeThread(*iter);
		//clear postponed threads' corresponding metas
		// delete thd_metas_map_[*iter];
		// thd_metas_map_.erase(*iter);
	}
}

inline 
void Verifier::ClearPStmtCorrespondingMetas(PStmt *pstmt,MetaSet *metas)
{
	DEBUG_ASSERT(pstmt && metas);
	if(pstmt_metas_map_.find(pstmt)==pstmt_metas_map_.end() ||
		pstmt_metas_map_[pstmt]==NULL)
		return ;
	MetaSet *pstmt_metas=pstmt_metas_map_[pstmt];
	for(MetaSet::iterator iter=metas->begin();iter!=metas->end();iter++) {
		if(pstmt_metas->find(*iter)!=pstmt_metas->end())
			pstmt_metas->erase(*iter);
	}
	if(pstmt_metas->empty()) {
		delete pstmt_metas;
		pstmt_metas_map_[pstmt]=NULL;
	}
}

/**
 * 
 */
void Verifier::RacedMeta(PStmt *first_pstmt,address_t start_addr,address_t end_addr,
	PStmt *second_pstmt,Inst *inst,thread_t curr_thd_id,RaceEventType type,
	PostponeThreadSet &pp_thds)
{
	if(pstmt_metas_map_.find(first_pstmt)==pstmt_metas_map_.end() || 
		pstmt_metas_map_[first_pstmt]==NULL)
		return ;
	MetaSet *first_metas=pstmt_metas_map_[first_pstmt];
	MAP_KEY_NOTFOUND_NEW(pstmt_metas_map_,second_pstmt,MetaSet);
	MAP_KEY_NOTFOUND_NEW(thd_metas_map_,curr_thd_id,MetaSet);
	MetaSet *second_metas=pstmt_metas_map_[second_pstmt];

	bool flag=false;
 	timestamp_t curr_thd_clk=thd_vc_map_[curr_thd_id]->GetClock(curr_thd_id);

	for(address_t iaddr=start_addr;iaddr<end_addr;iaddr+=unit_size_) {
		Meta *meta=GetMeta(iaddr);
		DEBUG_ASSERT(meta);
		//
		if(first_metas->find(meta)!=first_metas->end()) {
			for(ThreadMetasMap::iterator iter=thd_metas_map_.begin();
				iter!=thd_metas_map_.end();iter++) {
				//check meta only in postponed thread
				if(pp_thd_set_.find(iter->first)!=pp_thd_set_.end() && 
					iter->second!=NULL && 
					iter->second->find(meta)!=iter->second->end()) {
					//traverse current thread's last snapshot
					MetaSnapshot &meta_ss=meta->meta_ss_map[iter->first]->back();
					//filter raced inst pair
					if(meta->RacedInstPair(meta_ss.inst,inst))
						continue ;
					if(type==RACE_EVENT_WRITE) {
						flag=true;
						pp_thds.insert(iter->first);
						if(meta_ss.type==RACE_EVENT_WRITE)
							PrintDebugRaceInfo(meta,WRITETOWRITE,iter->first,
								meta_ss.inst,curr_thd_id,inst);
						else
							PrintDebugRaceInfo(meta,READTOWRITE,iter->first,
								meta_ss.inst,curr_thd_id,inst);
						meta->AddRacedInstPair(meta_ss.inst,inst);
					}
					else if(type==RACE_EVENT_READ) {
						if(meta_ss.type==RACE_EVENT_WRITE) {
							flag=true;
							pp_thds.insert(iter->first);
							PrintDebugRaceInfo(meta,WRITETOREAD,iter->first,
								meta_ss.inst,curr_thd_id,inst);
							meta->AddRacedInstPair(meta_ss.inst,inst);
						}
					}
				}
				//
				if(iter->first!=curr_thd_id && iter->second!=NULL && 
					iter->second->find(meta)!=iter->second->end()) {

					//traverse current thread history snapshot
					MetaSnapshotVector *meta_ss_vec=meta->meta_ss_map[iter->first];
					timestamp_t thd_clk=
					 	thd_vc_map_[curr_thd_id]->GetClock(iter->first);
					for(MetaSnapshotVector::iterator iiter=meta_ss_vec->begin();
						iiter!=meta_ss_vec->end();iiter++) {
						if(meta->RacedInstPair(iiter->inst,inst))
							continue ;

						if(type==RACE_EVENT_WRITE && iiter->thd_clk>thd_clk) {
							flag=true;
							if(iiter->type==RACE_EVENT_WRITE)
								PrintDebugRaceInfo(meta,WRITETOWRITE,iter->first,
									iiter->inst,curr_thd_id,inst);
							else
								PrintDebugRaceInfo(meta,READTOWRITE,iter->first,
									iiter->inst,curr_thd_id,inst);
							meta->AddRacedInstPair(iiter->inst,inst);
						}

						if(type==RACE_EVENT_READ && iiter->thd_clk>thd_clk) {
							if(iiter->type==RACE_EVENT_WRITE) {
								flag=true;
								PrintDebugRaceInfo(meta,WRITETOREAD,iter->first,
									iiter->inst,curr_thd_id,inst);
								meta->AddRacedInstPair(iiter->inst,inst);
							}
						}
					}
				}
			}
 		}
 		//add current thread's snapshot
 		meta->AddMetaSnapshot(curr_thd_id,MetaSnapshot(curr_thd_clk,type,inst));
		second_metas->insert(meta);
		thd_metas_map_[curr_thd_id]->insert(meta);
	}
	//
	if(flag)
		prace_db_->RemoveRelationMapping(first_pstmt,second_pstmt);
}

void Verifier::PrintDebugRaceInfo(Meta *meta,RaceType race_type,thread_t t1,
	Inst *i1,thread_t t2,Inst *i2)
{
	const char *race_type_name;
	switch(race_type) {
		case WRITETOREAD:
			race_type_name="WAR";
			break;
		case WRITETOWRITE:
			race_type_name="WAW";
			break;
		case READTOWRITE:
			race_type_name="RAW";
			break;
		case WRITETOREAD_OR_READTOWRITE:
			race_type_name="RAW|WAR";
			break;
		default:
			break;
	}
	DEBUG_FMT_PRINT_SAFE("%s%s\n",SEPARATOR,SEPARATOR);
	DEBUG_FMT_PRINT_SAFE("%s race detected \n",race_type_name);
	DEBUG_FMT_PRINT_SAFE("  addr = 0x%lx\n",meta->addr);
	DEBUG_FMT_PRINT_SAFE("  first thread = [%lx] , inst = [%s]\n",t1,
		i1->ToString().c_str());
	DEBUG_FMT_PRINT_SAFE("  second thread = [%lx] , inst = [%s]\n",t2,
		i2->ToString().c_str());
	DEBUG_FMT_PRINT_SAFE("%s%s\n",SEPARATOR,SEPARATOR);
}

}// namespace race