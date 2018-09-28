#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <stdint.h>
#include <map>
#include <vector>
#include <mhash.h>
#include <assert.h>
#include <time.h>
#include <fstream>
#include <sstream>
#include <set>
#include <bitset>
using namespace std;

int  CACHE_LINE_SIZE;	// cache每行元素个数
#define CACHE_LINE_NUM 1	// cache行数

#define FIFO 1       //先进先出（First In，First Out）
#define RANDOM 2
#define LRU 3        //Least recently used，最近最少使用
#define LFU 4        //Least Frequently Used
#define ADT 5
#define ADT2 6
#define BTE 7        //benefit to elephant,即RRTC
#define MICE 11
#define ELEPHANT 12
#define RABBIT 13
#define Null -1

FILE *fp;
FILE *fp_stat;
int cur_policy;	// 当前的替换策略
int num = 0;

// 流的五元组
typedef struct _header header_t;
struct _header {
	u_int 	sip;
	u_int 	dip;
	u_short sp;
	u_short dp;
	u_char 	prot;
	
	// 运算符重载，用于map插入比较
	bool operator < (const header_t &a) const
 	{
		if (sip > a.sip) return false;
		if (sip < a.sip) return true;
		// below are sip == a.sip
		if (dip > a.dip) return false;
 		if (dip < a.dip) return true;
		// below are dip == a.dip
 		if (sp > a.sp) return false;
 		if (sp < a.sp) return true;
		// below are sp == a.sp
 		if (dp > a.dp) return false;
 		if (dp < a.dp) return true;
		// below are dp == a.dp
 		if (prot < a.prot) return true;
 		else return false;
  	}

  	bool operator == (const header_t &a) const
  	{
  		if (sip == a.sip && dip == a.dip && sp == a.sp && dp == a.dp && prot == a.prot)
  			return true;
  		else
  			return false;
  	}
};

// 每个cache结点
typedef struct { 
	header_t tuple;		// 五元组
	u_char 	valid;		// cache结点是否有效，如一开始cold start时，all valid = 0
	int 	index_i;	// cache结点所在cache矩阵中的位置
	int 	index_j;
} cache_node_t;

typedef struct {
	int mice;
	int rabbit;
	int elephant;
} evict_info_t;

// cache行的信息
typedef struct cache_line {
	int valid_node_num;	// 该cache行中有效结点的数目（用于cold start完成的条件判断）
	int evict_times;
	int lived_time;   //该cache行的存活时间
} cache_line_t;

struct final_stat {
	int times[32];
};

// 每个流的信息，保存在flow_table结构中（正反两个方向的流合并成同一个流，这里的流更恰当说应该为连接）
typedef struct {		
	u_int len;		// 流的长度（数据包个数）
	u_int evict_times;	// 流发生cache替换的次数
	u_int hit_times;	// 流命中的总次数
	u_int cur_hit_times;    //流在cache中的命中次数
	u_int live_times;       //流在cache中的存活时间
	u_int clock_flag;       //时钟标志
	int flow_type;		// 流的类型 ELEPHANT MICE RABBIT
	int cache_line_evict_times;
	u_int sample_len;	// #packets of the flow in current sample window
	u_int sampe_hits;	// #hits of the flow in current sample window
} flow_stat_t;


map<header_t, flow_stat_t> pre_flow_table;	// 预处理流表结构，用于统计流的类型

vector<vector<cache_node_t> > cache;
map<header_t, flow_stat_t> flow_table;	// 流表结构
cache_line_t cache_line[CACHE_LINE_NUM];	// 每行cache的统计信息结构
map<u_int, evict_info_t> evict_stat;
struct final_stat burst_stat;

void dump_flow_stats();

/*
 * ------------------------------------------------------------------------------
 * my new policy, use real tarffic characteristics
 */
struct nf_counter{
	int Fsum;
	int Csum;
};

nf_counter NF_Counter;
map<header_t, int> NF_Table;  //(f, c)
map<header_t, int> HH_Table; //(f, c)
const int sampleSize = 2000;  //师兄设置为1000
const int hhSize = 500;
const int maxC = 50;
int updateTime = 0;
int x = 0; //标志位

/*
 * map不是严格O（1）
 */
int check( header_t f)
{
	int x = 0;
	static uint64_t slamp = 1;
	//检查标志位
	if(HH_Table.count(f)){ //在HH_Table中命中
		x = 1;
		//if(HH_Table[f] < maxC)
		HH_Table[f] += 1;
	}else{
		x = 0;
		NF_Counter.Csum += 1;
		if(NF_Table.count(f)){//在NF_Table中命中
			//if(NF_Table[f] < maxC)
				NF_Table[f] += 1;
		}else{
			NF_Table[f] = 1;
			NF_Counter.Fsum += 1;
		}
	}
	//更新HH_Table和NF_Table
	if(slamp % sampleSize == 0){
		int d =  (NF_Counter.Fsum == 0 ? 0 : NF_Counter.Csum / NF_Counter.Fsum); //计算衰减因子
		//cout<<NF_Counter.Csum<<'\t'<<NF_Counter.Fsum<<endl;
		for(auto iterHH = HH_Table.begin(); iterHH != HH_Table.end(); ){//更新HH_Table,移除c = 0的项
			//cout << "HH_Table heavy hitters: "<<iterHH->first.sip<<"\n";
			if (iterHH->second <= 2*d ) {
				HH_Table.erase(iterHH++);
			}
			else {
				iterHH->second -= 2*d;
				iterHH++;
			}
			//if(iterHH->second <= 2 * d){
				//HH_Table.erase(iterHH++);
			//}else{
				//iterHH->second -= 2 * d;
				//iterHH++;
			//}
		}
		for(auto iterNF = NF_Table.begin(); iterNF != NF_Table.end(); iterNF++){//更新HH_Table，插入新的项
			if(HH_Table.size() >= hhSize) break;
			//if (iterNF->second > d) HH_Table[iterNF->first] = iterNF->second - d;
			if(iterNF->second > d) HH_Table[iterNF->first] = iterNF->second - d;
		}
		NF_Table.clear(); //清空NF_Table
		NF_Counter.Csum = NF_Counter.Fsum = 0; //清空NF_Counter
	}

	slamp++;
	return x;
}


/*
 * 直接hash，严格O(1)
 */
vector<uint> NF_Hash(2000, 0);  //(f, c)
map<uint, uint> HH_Hash; //(f, c)
u_int tuple_hash(const header_t *tuple, u_int number);


// given an incoming packet of flow[idx], update NF table, HH table and counters
int update_tables(uint idx)
{
	//检查标志位
	if(HH_Hash.count(idx)) { //在HH_Hash中命中
		HH_Hash[idx] += 1;
		return 1;
	} else {
		NF_Counter.Csum += 1;
		if(NF_Hash[idx]) {	//在NF_Table中命中
			NF_Hash[idx] += 1;
		} else {
			NF_Hash[idx] = 1;
			NF_Counter.Fsum += 1;
		}
		return 0;
	}
}


void dump_elephants()
{
	map<header_t, flow_stat_t>::iterator it;
	header_t	*header;
	uint		idx;

	printf("elephants: idx : len\n");
	for (it = pre_flow_table.begin(); it != pre_flow_table.end(); it++) {
		if (it->second.len < 20)	// mice flow
			continue;
		//elephant flow
		idx = tuple_hash(&it->first, 800);
		printf("%3d : %4d\n", idx, it->second.len);
	}
}


void dump_hh_table()
{
}


// statistics of flows in current sample window
void sample_win_stats()
{
	// dump the HH table
	// dump stats for elephant flows
}


// at the completion of a sample window, update NF table and HH table
int update_sample_win()
{
	int d =  (NF_Counter.Fsum == 0 ? 0 : NF_Counter.Csum / NF_Counter.Fsum); //计算衰减因子

	for(auto iterHH = HH_Hash.begin(); iterHH != HH_Hash.end(); ){//更新HH_Table,移除c = 0的项
		if(iterHH->second <= 2*d ) {
			HH_Hash.erase(iterHH++);
		} else {
			iterHH->second -= 2*d;
			iterHH++;
		}
	}
	for(int i = 0; i < NF_Hash.size(); i++) {//更新HH_Table，插入新的项
		if(HH_Hash.size() >= hhSize) break;
		if(NF_Hash[i] > d) HH_Hash[i] = NF_Hash[i] - d;
	}
	NF_Hash.clear(); //清空NF_Table
	NF_Hash.resize(2000, 0);
	NF_Counter.Csum = NF_Counter.Fsum = 0; //清空NF_Counter
}


int check_hash(header_t f, int nsamples)
{
	uint idx = tuple_hash(&f, 800);
	update_tables(idx);

	//更新HH_Table和NF_Table
	if(nsamples % sampleSize == 0) 
		update_sample_win();
}


int loc_clock[CACHE_LINE_NUM];
void cache_init(int policy)
{
	cur_policy = policy; //init the policy
	int i, j;
	for (i = 0; i < CACHE_LINE_NUM; i++) {
		for (j = 0; j < CACHE_LINE_SIZE; j++) {
			cache[i][j].valid = 0;
			cache[i][j].index_i = i;
			cache[i][j].index_j = j;
		}
		cache_line[i].lived_time = 0;
	}
}


int get_random(int max) // return a number ranging from 0 - max-1
{
	return rand() % max;
}


void reverse(header_t *t)
{
	if (t->sip >= t->dip)
		return;

	t->sip ^= t->dip;
	t->dip ^= t->sip;
	t->sip ^= t->dip;
	t->sp ^= t->dp;
	t->dp ^= t->sp;
	t->sp ^= t->dp;
}


u_int tuple_hash(const header_t *tuple, u_int number)	// 采用md5算法将五元组哈希到cache行
{
	MHASH td;
	u_char hash[16];
	td = mhash_init(MHASH_MD5);
	if (td == MHASH_FAILED)
		exit(1);
	mhash(td, tuple, sizeof(header_t));
	mhash_deinit(td, hash);
	u_int *integer = (u_int *)hash;
	return (*integer) % number;
}


int flow_table_query(header_t *tuple)	// 查询流表
{
	map<header_t, flow_stat_t>::iterator iter = flow_table.find(*tuple);
	return (iter == flow_table.end()) ? 0 : 1;
}


int pre_flow_table_query(header_t *tuple)	// 查询流表
{
	map<header_t, flow_stat_t>::iterator iter = pre_flow_table.find(*tuple);

	if (iter == pre_flow_table.end()) {
		return 0;
	} else {
		return 1;
	}
}


int evict_query(u_int evict_times)	// 查询流表
{
	map<u_int, evict_info_t>::iterator iter = evict_stat.find(evict_times);
	return (iter == evict_stat.end()) ? 0 : 1;
}


void rotate(int loc_from, int loc_to, u_int addr)
{
	int i;
	cache_node_t temp;
	temp.tuple.sip = cache[addr][loc_from].tuple.sip;
	temp.tuple.dip = cache[addr][loc_from].tuple.dip;
	temp.tuple.sp = cache[addr][loc_from].tuple.sp;
	temp.tuple.dp = cache[addr][loc_from].tuple.dp;
	temp.tuple.prot = cache[addr][loc_from].tuple.prot;
	temp.valid = cache[addr][loc_from].valid;

	for (i = loc_from; i > loc_to; i--) {
		cache[addr][i].valid = cache[addr][i-1].valid;
		cache[addr][i].tuple.sip = cache[addr][i-1].tuple.sip;
		cache[addr][i].tuple.dip = cache[addr][i-1].tuple.dip;
		cache[addr][i].tuple.sp = cache[addr][i-1].tuple.sp;
		cache[addr][i].tuple.dp = cache[addr][i-1].tuple.dp;
		cache[addr][i].tuple.prot = cache[addr][i-1].tuple.prot;
	}

	cache[addr][loc_to].valid = temp.valid;
	cache[addr][loc_to].tuple.sip = temp.tuple.sip;
	cache[addr][loc_to].tuple.dip = temp.tuple.dip;
	cache[addr][loc_to].tuple.sp = temp.tuple.sp;
	cache[addr][loc_to].tuple.dp = temp.tuple.dp;
	cache[addr][loc_to].tuple.prot = temp.tuple.prot;
}


int cache_query(header_t *tuple)	// 查询cache
{
	u_int cache_addr = tuple_hash(tuple, CACHE_LINE_NUM);
	int hit = 0;
	int i;
	int hit_loc = 0;
	for (i = 0; i < CACHE_LINE_SIZE; i++) {
		if (cache[cache_addr][i].valid && cache[cache_addr][i].tuple == *tuple)	{ // 元素有效且五元组相同，命中
			hit = 1;
			hit_loc = i;
			flow_table[*tuple].hit_times++;	// 该流的命中次数增加
			flow_table[*tuple].cur_hit_times++;
			break;
		}
	}
	if (flow_table_query(tuple))
		flow_table[*tuple].len++;	// 如果在流表中能找到，则增加流中包的个数
	if (hit == 1) {
		// hit adjustment
		switch (cur_policy) {
		case FIFO:
			break;
		case RANDOM:
			break;
		case LFU:
			break;
		case LRU:
			updateTime++;
			rotate(hit_loc, 0, cache_addr);	// 命中的交换到cache首部
			break;
		case ADT:
			break;
		case ADT2:
			if(flow_table[*tuple].evict_times <=5 );
			else if(flow_table[*tuple].evict_times <=100)
				rotate(hit_loc, 4, cache_addr);
			else
				rotate(hit_loc, 0, cache_addr);
			break;
		case BTE:
			if(x == 1) {
				rotate(hit_loc, 0, cache_addr);	// 命中的交换到cache首部
				updateTime++;
			} else {
				//rotate(hit_loc, 0, cache_addr);
			}
			//rotate(hit_loc, 0, cache_addr);	// 命中的交换到cache首部
			break;
		default:
			break;
		}
	} else {
		cache_line[cache_addr].evict_times++;
	}

	return hit;
}


void flow_table_insert(header_t *tuple)	// 在流表中插入一个新结点
{
	flow_stat_t *info = (flow_stat_t *) malloc(sizeof(flow_stat_t));
	info->len = 0;	// 初始化
	info->evict_times = 0;
	info->hit_times = 0;
	info->cur_hit_times = 0;
	info->live_times = 0;
	info->clock_flag = 0;
	info->cache_line_evict_times = 0;

	pair<map<header_t, flow_stat_t>::iterator, bool> inserted;
	inserted = flow_table.insert(map<header_t, flow_stat_t>::value_type(*tuple, *info));
	if (true != inserted.second) {
		printf("insert failed!\n");	// 插入失败
	} else {
		flow_table[*tuple].len++;	// 插入成功，增加该流包个数
	}
}


void pre_flow_table_insert(header_t *tuple)	// 在流表中插入一个新结点
{
	flow_stat_t *info = (flow_stat_t *)malloc(sizeof(flow_stat_t));
	info->len = 0;	// 初始化
	info->evict_times = 0;
	info->hit_times = 0;
	info->cur_hit_times = 0;
	info->live_times = 0;
	info->clock_flag = 0;
	info->cache_line_evict_times = 0;

	pair<map<header_t, flow_stat_t>::iterator, bool> inserted;
	inserted = pre_flow_table.insert(map<header_t, flow_stat_t>::value_type(*tuple, *info));
	if (true != inserted.second) {
		printf("insert failed!\n");	// 插入失败
	} else {
		pre_flow_table[*tuple].len++;	// 插入成功，增加该流包个数
	}
}


void evict_insert(u_int evict_times)
{
	evict_info_t *info = (evict_info_t *)malloc(sizeof(evict_info_t));
	info->mice = 0;
	info->rabbit = 0;
	info->elephant = 0;

	pair<map<u_int, evict_info_t>::iterator, bool> inserted;
	inserted = evict_stat.insert(map<u_int, evict_info_t>::value_type(evict_times, *info));
	if (true != inserted.second) {
		printf("insert failed!\n");	// 插入失败
	}
}


void flow_table_update(header_t *tuple)	// 每次从cache中被替换出的元素被送入流表，并更新流表信息，此处更新换入换出次数
{
	if (flow_table_query(tuple)) {
		flow_table[*tuple].evict_times++;
	} else {
		printf("updating a flow not existed!\n");
	}
}


void pre_flow_table_update(header_t *tuple)
{
	if (pre_flow_table_query(tuple)) {
		pre_flow_table[*tuple].len++;
	} else {
		printf("updating a flow not existed!\n");
	}
}


void replace(int loc, header_t *swap_in, header_t *swap_out, u_int addr)	// 将loc位置的元素赋给swap_out换出，换入swap_in
{
	cache_node_t *node_cursor = &cache[addr][loc];
	swap_out->sip = node_cursor->tuple.sip;
	swap_out->dip = node_cursor->tuple.dip;
	swap_out->sp = node_cursor->tuple.sp;
	swap_out->dp = node_cursor->tuple.dp;
	swap_out->prot = node_cursor->tuple.prot;
	node_cursor->tuple.sip = swap_in->sip;
	node_cursor->tuple.dip = swap_in->dip;
	node_cursor->tuple.sp = swap_in->sp;
	node_cursor->tuple.dp = swap_in->dp;
	node_cursor->tuple.prot = swap_in->prot;
}


void insert_blank(header_t *swap_in, u_int addr)	// cache行cold start时，逐渐将其填满
{
	int i;
	cache_node_t *node_cursor;
	for (i = 0; i < CACHE_LINE_SIZE ; i++) {
		node_cursor = &cache[addr][i];
		if (!node_cursor->valid)
			break;
	}
	node_cursor->tuple.sip = swap_in->sip;
	node_cursor->tuple.dip = swap_in->dip;
	node_cursor->tuple.sp = swap_in->sp;
	node_cursor->tuple.dp = swap_in->dp;
	node_cursor->tuple.prot = swap_in->prot;
	node_cursor->valid = 1;
	cache_line[addr].valid_node_num++;
}


void delete_entity(u_int addr)  //将cache行清空
{
	int i;
	cache_node_t *node_cursor;
	for (i = 0; i < CACHE_LINE_SIZE; i++) {
		node_cursor = &cache[addr][i];
		if (node_cursor->valid) {
			node_cursor->valid = 0;
			flow_table_update(&node_cursor->tuple);
		}
	}
	cache_line[addr].valid_node_num = 0;
}


void insert(int loc, header_t *swap_in, header_t *swap_out, u_int addr)
{
	cache_node_t *node_cursor = &cache[addr][CACHE_LINE_SIZE-1];
	swap_out->sip = node_cursor->tuple.sip;
	swap_out->dip = node_cursor->tuple.dip;
	swap_out->sp = node_cursor->tuple.sp;
	swap_out->dp = node_cursor->tuple.dp;
	swap_out->prot = node_cursor->tuple.prot;

	for (int i = CACHE_LINE_SIZE - 1; i > loc; i--) {
		cache[addr][i].valid = cache[addr][i-1].valid;
		cache[addr][i].tuple.sip = cache[addr][i-1].tuple.sip;
		cache[addr][i].tuple.dip = cache[addr][i-1].tuple.dip;
		cache[addr][i].tuple.sp = cache[addr][i-1].tuple.sp;
		cache[addr][i].tuple.dp = cache[addr][i-1].tuple.dp;
		cache[addr][i].tuple.prot = cache[addr][i-1].tuple.prot;
	}

	cache[addr][loc].valid = 1;
	cache[addr][loc].tuple.sip = swap_in->sip;
	cache[addr][loc].tuple.dip = swap_in->dip;
	cache[addr][loc].tuple.sp = swap_in->sp;
	cache[addr][loc].tuple.dp = swap_in->dp;
	cache[addr][loc].tuple.prot = swap_in->prot;

	if (cache_line[addr].valid_node_num != CACHE_LINE_SIZE) {
		cache_line[addr].valid_node_num++;
	}
}


int cache_swap(header_t *swap_in, header_t *swap_out)	// cache替换
{
	u_int cache_addr = tuple_hash(swap_in, CACHE_LINE_NUM);
	int evict = (cache_line[cache_addr].valid_node_num == CACHE_LINE_SIZE); // 如果cold start阶段已经结束，则将evict标记置1
	int min_evict_times,max_evict_times;
	int min_cur_hit_times;
	min_evict_times = max_evict_times = flow_table[cache[cache_addr][0].tuple].evict_times;
	min_cur_hit_times = flow_table[cache[cache_addr][0].tuple].cur_hit_times;
	int i, vol = 0, vol2 = 0;

	if(evict == 0) {
		insert_blank(swap_in, cache_addr);
		return evict;
	}

	switch (cur_policy) {
		case FIFO:
			insert(0, swap_in, swap_out, cache_addr);
			break;
		case RANDOM:
			replace(get_random(CACHE_LINE_SIZE), swap_in, swap_out, cache_addr);	// 随机替换
			break;
		case LFU:
			for(i = 1; i < CACHE_LINE_SIZE; i++) {
				if(flow_table[cache[cache_addr][i].tuple].cur_hit_times < min_cur_hit_times) {
					min_cur_hit_times = flow_table[cache[cache_addr][i].tuple].cur_hit_times;
					vol = i;
				}

			}
			replace(vol, swap_in, swap_out, cache_addr);
			break;
		case LRU:
			insert(0, swap_in, swap_out, cache_addr);
			break;
		case ADT:
			cache_line[cache_addr].lived_time++;
			//方案1
			/*if(cache_line[cache_addr].lived_time == 1024)
			{
				cache_line[cache_addr].lived_time = 0;
				delete_entity(cache_addr);
				insert_blank(swap_in, cache_addr);
				return 0;
			}*/
			//方案2
			for(i = 1; i < CACHE_LINE_SIZE-2; i++) {
				flow_table[cache[cache_addr][i].tuple].live_times++;
				if(flow_table[cache[cache_addr][i].tuple].live_times == 256) {
					flow_table[cache[cache_addr][i].tuple].live_times = 0;
					replace(i, swap_in, swap_out, cache_addr);
					return evict;
				}
				if(flow_table[cache[cache_addr][i].tuple].evict_times < min_evict_times) {
					min_evict_times = flow_table[cache[cache_addr][i].tuple].evict_times;
					vol = i;
				}

			}
			replace(vol, swap_in, swap_out, cache_addr);
			break;
		case ADT2:
			if(flow_table[*swap_in].evict_times <=5 ) {
				replace(CACHE_LINE_SIZE - 1, swap_in, swap_out, cache_addr);
			} else if(flow_table[*swap_in].evict_times <= 100) {
				insert(4, swap_in, swap_out, cache_addr);
			} else {
				flow_table[*swap_in].live_times = 1;
				insert(0, swap_in, swap_out, cache_addr);
			}
			if(flow_table[*swap_out].live_times > 0)
				flow_table[*swap_out].live_times--;
			if(flow_table[*swap_out].live_times > 0) {
				*swap_in = *swap_out;
				insert(0, swap_in, swap_out, cache_addr);
			}
			break;
		case BTE:
			if(x == 1) {
				insert(0, swap_in, swap_out, cache_addr);
			} else {
				insert(CACHE_LINE_SIZE - 1, swap_in, swap_out, cache_addr);
			}
			break;
		default:
			break;
	}
	flow_table[*swap_out].cur_hit_times = 0;
	return evict;
}


void stat()	// 统计
{
	u_int figure1_packet[9];
	u_int figure1_flow[9];
	u_int figure1_evict[9];
	int i;

	for (i = 0; i < 9; i++) {
		figure1_packet[i] = 0;
		figure1_flow[i] = 0;
		figure1_evict[i] = 0;
	}
	u_int evict_sum = 0;
	u_int hit_sum = 0;
	u_int len_sum = 0;
	u_int a = 0, b = 0, c = 0;
	// traverse flow table
	map<header_t, flow_stat_t>::iterator it;
	for (it = flow_table.begin(); it != flow_table.end(); it++) {
		evict_sum += it->second.evict_times;
		hit_sum += it->second.hit_times;
		len_sum += it->second.len;

		if (it->second.len == 1) {
			figure1_packet[0] += it->second.len;
			figure1_flow[0]++;
			figure1_evict[0] += it->second.evict_times;
		} else if (it->second.len <= 5) {
			figure1_packet[1] += it->second.len;
			figure1_flow[1]++;
			figure1_evict[1] += it->second.evict_times;
		} else if (it->second.len <= 20) {
			figure1_packet[2] += it->second.len;
			figure1_flow[2]++;
			figure1_evict[2] += it->second.evict_times;
		} else if (it->second.len <= 50) {
			figure1_packet[3] += it->second.len;
			figure1_flow[3]++;
			figure1_evict[3] += it->second.evict_times;
		}
		else if (it->second.len <= 100) {
			figure1_packet[4] += it->second.len;
			figure1_flow[4]++;
			figure1_evict[4] += it->second.evict_times;
		} else if (it->second.len <= 200) {
			figure1_packet[5] += it->second.len;
			figure1_flow[5]++;
			figure1_evict[5] += it->second.evict_times;
		} else if (it->second.len <= 500) {
			figure1_packet[6] += it->second.len;
			figure1_flow[6]++;
			figure1_evict[6] += it->second.evict_times;
		} else if (it->second.len <= 1000) {
			figure1_packet[7] += it->second.len;
			figure1_flow[7]++;
			figure1_evict[7] += it->second.evict_times;
		} else {
			figure1_packet[8] += it->second.len;
			figure1_flow[8]++;
			figure1_evict[8] += it->second.evict_times;
		}

		if (it->second.len < 20) {
			a += it->second.evict_times;  //a小鼠流驱逐次数
		} else {
			b += it->second.evict_times;  //b大象流驱逐次数
		}

		if (!evict_query(it->second.evict_times)) {
			evict_insert(it->second.evict_times);
		} else {
			if (it->second.len < 4) {
				evict_stat[it->second.evict_times].mice += it->second.evict_times;
			} else if (it->second.len < 20) {
				evict_stat[it->second.evict_times].rabbit += it->second.evict_times;
			} else {
				evict_stat[it->second.evict_times].elephant += it->second.evict_times;
			}
		}
	}
	printf("cache size num = %d\n", CACHE_LINE_SIZE);
	printf("updateTime = %d\n", updateTime);
	printf("hitTime = %d\n", hit_sum);    //缓存命中次数
	printf("hit rate = %f\n", (double)hit_sum/len_sum);
	/*
	printf("packet_range        flow_sum       packet_sum     total_evict_times\n");
	printf("         [1]        %u             %u             %u\n", figure1_flow[0], figure1_packet[0], figure1_evict[0]);
	printf("       [2,5]        %u             %u             %u\n", figure1_flow[1], figure1_packet[1], figure1_evict[1]);
	printf("      [6,20]        %u             %u             %u\n", figure1_flow[2], figure1_packet[2], figure1_evict[2]);
	printf("     [21,50]        %u             %u             %u\n", figure1_flow[3], figure1_packet[3], figure1_evict[3]);
	printf("    [51,100]        %u             %u             %u\n", figure1_flow[4], figure1_packet[4], figure1_evict[4]);
	printf("   [101,200]        %u             %u             %u\n", figure1_flow[5], figure1_packet[5], figure1_evict[5]);
	printf("   [201,500]        %u             %u             %u\n", figure1_flow[6], figure1_packet[6], figure1_evict[6]);
	printf("  [501,1000]        %u             %u             %u\n", figure1_flow[7], figure1_packet[7], figure1_evict[7]);
	printf("    [1000,+]        %u             %u             %u\n", figure1_flow[8], figure1_packet[8], figure1_evict[8]);
	*/

	//printf("a = %u, b = %u\n", a, b);

	/*map<u_int, evict_info_t>::iterator it2;
	for (it2 = evict_stat.begin(); it2 != evict_stat.end(); it2++)
	{
		fprintf(fp_stat, "%u, %d, %d, %d\n", it2->first, it2->second.mice, it2->second.rabbit, it2->second.elephant);
	}*/

	/*for (i = 0; i < 32; i++)
	{
		fprintf(fp_stat, "[%d-%d), %d\n", i*4, (i+1)*4, burst_stat.times[i]);
	}*/

}

/*
 * 构造trace
 */
void rebuild()
{
	ofstream out;
	string filename = "rebuild";
	out.open(filename.c_str());
	int i = 0;
	int k = 0;
	vector<vector<header_t> > ele(10);
	auto it = pre_flow_table.begin();
	for(; it != pre_flow_table.end(); it++){
		int k = i / 500;
		if(k == 1) break;
		ele[k].push_back(it->first);
		i++;
	}
	i = 0;
	int MOD = 150;
	for (; it != pre_flow_table.end(); it++){
		header_t tuplle = it->first;
		if(i % MOD == 0){
			
			MOD = rand() % (500 - 150) + 150;
			int hhn = rand() % (ele[k].size() - (int)(0.9 * ele[k].size())) + (int)(0.9 * ele[k].size());
			//int hhn = ele[k].size();

			vector<int> vc;
			vector<bool> fl(ele[k].size(), false);
			while(vc.size() < ele[k].size()){
				int c = rand() % ele[k].size();
				if(!fl[c]){
					vc.push_back(c);
					fl[c] = true;
				}
			}
			for(int j = 0; j < hhn; j++){
				for(int t = 0; t < 5; t++){
                    			header_t x = ele[k][vc[j]];
					out.write((char*)&x,sizeof(x));
				}
			}
		}
		i++;
		int c = rand() % ele[k].size();
		int rate = rand() % 100;
		if(rate < 50){
			header_t x = ele[k][c];
			out.write((char*)&x,sizeof(x));
		}else{
			out.write((char*)&tuplle,sizeof(header_t));
			//out.write((char*)&tuplle,sizeof(header_t));
		}
	}
	out.close();
}


int main(int argc, char *argv[] )
{
	int kind[4] = {3, 7};
	int nsamples = 0;

	//vector<int> cacheList = {100, 200, 300, 400, 500};
	vector<int> cacheList = {100, 200};
	for (int cc = 0; cc < cacheList.size(); cc++) { 
		for(int kc = 0; kc < 2; kc++ ) {
			//initial
			CACHE_LINE_SIZE = cacheList[cc];
			pre_flow_table.clear();
			flow_table.clear();
			evict_stat.clear();
			cache = vector<vector<cache_node_t> >(CACHE_LINE_NUM, vector<cache_node_t>(CACHE_LINE_SIZE));

			HH_Table.clear();
			NF_Table.clear();
			NF_Counter.Csum = NF_Counter.Fsum = 0;
			HH_Hash.clear();
			NF_Hash.clear();
			NF_Hash.resize(2000, 0);
			updateTime = 0;
			srand((unsigned)time(NULL));	// 产生md5 hash中所用伪随机数种子

			fp = fopen(argv[1], "rb");
			header_t pre_tuple;
			unsigned int tmp1, tmp2;
			// pre-processing
			while (fscanf(fp, "%d %d %d %d %d %u %u\n", &pre_tuple.sip, &pre_tuple.dip, 
						&pre_tuple.sp, &pre_tuple.dp, &pre_tuple.prot, &tmp1, &tmp2) != EOF) {
				//if (fread(&pre_tuple, sizeof(pre_tuple), 1, fp) != 1)	// eof
				//	break;
				reverse(&pre_tuple);
				if (!pre_flow_table_query(&pre_tuple))
					pre_flow_table_insert(&pre_tuple);
				pre_flow_table_update(&pre_tuple);
			}

			printf("flows: %d\n", pre_flow_table.size());
			dump_elephants();
			//rebuild();
			//return 0;
			rewind(fp);

			switch (kind[kc]) {
			case 3: 
				printf("LRU:\n");
				cache_init(LRU);
				break;
			case 7:
				printf("BTE:\n");
				cache_init(BTE);
				break;
			default:
				break;
			}

			header_t tuple;
			header_t swap_out;
			nsamples = 0;
			while (fscanf(fp, "%d %d %d %d %d %u %u\n", &tuple.sip, &tuple.dip, 
						&tuple.sp, &tuple.dp, &tuple.prot, &tmp1, &tmp2) != EOF) {
				//if (fread(&tuple, sizeof(tuple), 1, fp) != 1) // eof
				//   	break;
				reverse(&tuple);
				check_hash(tuple, ++nsamples);
				if (cache_query(&tuple))	// 如果cache hit, no need for subsequent processing
					continue;
				if (!flow_table_query(&tuple))	// 如果流表也miss，则证明是新流
					flow_table_insert(&tuple);	// 插入流表

				if (cache_swap(&tuple, &swap_out))	// swap_out tuple exists 如果cache行发生替换
					flow_table_update(&swap_out);	// 替换出的元素更新流表
			}

			printf("#flows: %d\n", flow_table.size());
			stat();
			dump_flow_stats();
			fclose(fp);
		}
	}
	return 0;
}


typedef struct {
	int	num_flows;
	int	evict_times;
	int	hit_times;
} table_stat_t;

void dump_flow_stats()
{
	table_stat_t	table_stats[32], *p;

	for (int i = 0; i < 32; i++) {
		table_stats[i].num_flows = 0;
		table_stats[i].evict_times = 0;
		table_stats[i].hit_times = 0;
	}

	map<header_t, flow_stat_t>::iterator it;
	for (it = flow_table.begin(); it != flow_table.end(); it++) {
		if (it->second.len < 32)
			p = &table_stats[it->second.len - 1];
		else
			p = &table_stats[31];
		
		p->num_flows++;
		p->evict_times += it->second.evict_times;
		p->hit_times += it->second.hit_times;
	}

	for (int i = 0; i < 32; i++) {
		if (table_stats[i].num_flows == 0)
			continue;
		printf("flows[%2d]: #%6d flows, %7d hits, %7d evicts\n", i+1, table_stats[i].num_flows, 
				table_stats[i].hit_times, table_stats[i].evict_times);
	}
	
}
