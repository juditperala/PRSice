/*
 * genotype.cpp
 *
 *  Created on: 27 Mar 2017
 *      Author: shingwanchoi
 */

#include "genotype.hpp"

std::mutex Genotype::clump_mtx;

void Genotype::init_chr(int num_auto, bool no_x, bool no_y, bool no_xy, bool no_mt)
{
	// this initialize haploid mask as the maximum possible number
	m_haploid_mask = new uintptr_t[CHROM_MASK_WORDS];
	fill_ulong_zero(CHROM_MASK_WORDS, m_haploid_mask);

	if(num_auto < 0)
	{
		num_auto = -num_auto;
		m_autosome_ct = num_auto;
		m_xymt_codes[X_OFFSET] = -1;
		m_xymt_codes[Y_OFFSET] = -1;
		m_xymt_codes[XY_OFFSET] = -1;
		m_xymt_codes[MT_OFFSET] = -1;
		m_max_code = num_auto;
		fill_all_bits(((uint32_t)num_auto) + 1, m_haploid_mask);
	}
	else
	{
		m_autosome_ct = num_auto;
		m_xymt_codes[X_OFFSET] = num_auto+1;
		m_xymt_codes[Y_OFFSET] = num_auto+2;
		m_xymt_codes[XY_OFFSET] = num_auto+3;
		m_xymt_codes[MT_OFFSET] = num_auto+4;
		set_bit(num_auto + 1, m_haploid_mask);
		set_bit(num_auto + 2, m_haploid_mask);
		if(no_x){
			m_xymt_codes[X_OFFSET] = -1;
			clear_bit(num_auto + 1, m_haploid_mask);
		}
		if(no_y)
		{
			m_xymt_codes[Y_OFFSET] = -1;
			clear_bit(num_auto + 2, m_haploid_mask);
		}
		if(no_xy)
		{
			m_xymt_codes[XY_OFFSET] = -1;
		}
		if(no_mt)
		{
			m_xymt_codes[MT_OFFSET] = -1;
		}
		if (m_xymt_codes[MT_OFFSET] != -1) {
			m_max_code = num_auto + 4;
		} else if (m_xymt_codes[XY_OFFSET] != -1) {
			m_max_code = num_auto + 3;
		} else if (m_xymt_codes[Y_OFFSET] != -1) {
			m_max_code = num_auto + 2;
		} else if (m_xymt_codes[X_OFFSET] != -1) {
			m_max_code = num_auto + 1;
		} else {
			m_max_code = num_auto;
		}
	}
	m_chrom_mask = new uintptr_t[CHROM_MASK_WORDS];
	fill_ulong_zero(CHROM_MASK_WORDS, m_chrom_mask);
	fill_all_bits(m_autosome_ct + 1, m_chrom_mask);
	for (uint32_t xymt_idx = 0; xymt_idx < XYMT_OFFSET_CT; ++xymt_idx) {
		int32_t cur_code = m_xymt_codes[xymt_idx];
		if (cur_code != -1) {
			set_bit(m_xymt_codes[xymt_idx], m_chrom_mask);
		}
	}
	m_chrom_start.resize(m_max_code);// 1 extra for the info
}

void Genotype::set_genotype_files(std::string prefix)
{
	if(prefix.find("#")!=std::string::npos)
	{
		for(size_t chr = 1; chr < m_max_code; ++chr)
		{
			std::string name = prefix;
			misc::replace_substring(name, "#", std::to_string(chr));
			m_genotype_files.push_back(name);
		}
	}
	else
	{
		m_genotype_files.push_back(prefix);
	}
}

Genotype::Genotype(std::string prefix, int num_auto,
		bool no_x, bool no_y, bool no_xy, bool no_mt, const size_t thread, bool verbose)
{
	m_xymt_codes.resize(XYMT_OFFSET_CT);
	init_chr(num_auto, no_x, no_y, no_xy, no_mt);
	m_thread = thread;
	set_genotype_files(prefix);
	m_sample_names = load_samples();
	m_existed_snps = load_snps();
	if(verbose)
	{
		fprintf(stderr, "%zu people (%zu males, %zu females) included\n", m_unfiltered_sample_ct, m_num_male, m_num_female);
		if(m_num_ambig!=0) fprintf(stderr, "%u ambiguous variants excluded\n", m_num_ambig);
		fprintf(stderr, "%zu variants included\n", m_marker_ct);
	}
	m_founder_ctl = BITCT_TO_WORDCT(m_founder_ct);
	m_founder_ctv3 = BITCT_TO_ALIGNED_WORDCT(m_founder_ct);
	m_founder_ctsplit = 3 * m_founder_ctv3;
}

Genotype::~Genotype() {
	// TODO Auto-generated destructor stub
	if(m_founder_info!=nullptr) delete [] m_founder_info;
	if(m_sex_male != nullptr) delete [] m_sex_male;
	if(m_sample_exclude != nullptr) delete [] m_sample_exclude;
	if(m_marker_exclude != nullptr) delete [] m_marker_exclude;
	if(m_haploid_mask != nullptr) delete [] m_haploid_mask;
	if(m_chrom_mask != nullptr) delete [] m_chrom_mask;
}

void Genotype::read_base(const Commander &c_commander, Region &region)
{
	// can assume region is of the same order as m_existed_snp
	m_scoring = c_commander.get_scoring();
	const std::string input = c_commander.base_name();
	const bool beta = c_commander.beta();
	const bool fastscore = c_commander.fastscore();
	const bool full = c_commander.full();
	std::vector<int> index = c_commander.index(); // more appropriate for commander
	// now coordinates obtained from target file instead. Coordinate information
	// in base file only use for validation
	std::ifstream snp_file;
	snp_file.open(input.c_str());
	if(!snp_file.is_open())
	{
		std::string error_message = "ERROR: Cannot open base file: " +input;
		throw std::runtime_error(error_message);
	}
	int max_index = index[+BASE_INDEX::MAX];
	std::string line;
	if (!c_commander.has_index()) std::getline(snp_file, line);

	// category related stuff
	double threshold = (c_commander.fastscore())? c_commander.bar_upper() : c_commander.upper();
	double bound_start = c_commander.lower();
	double bound_end = c_commander.upper();
	double bound_inter = c_commander.inter();

	threshold = (full)? 1.0 : threshold;
	std::vector < std::string > token;

	bool exclude = false;
	bool hap_error = false;
	// Some QC countss
	size_t num_duplicated = 0;
	size_t num_excluded = 0;
	size_t num_not_found = 0;
	size_t num_mismatched = 0;
	size_t num_not_converted = 0; // this is for NA
	size_t num_negative_stat = 0;

	std::unordered_set<std::string> dup_index;
	std::vector<int> exist_index; // try to use this as quick search
	// Actual reading the file, will do a bunch of QC
	while (std::getline(snp_file, line))
	{
		misc::trim(line);
		if (line.empty()) continue;
		exclude = false;
		token = misc::split(line);
		if (token.size() <= max_index) throw std::runtime_error("More index than column in data");
		else
		{
			std::string rs_id = token[index[+BASE_INDEX::RS]];

			if(m_existed_snps_index.find(rs_id)!=m_existed_snps_index.end() && dup_index.find(rs_id)==dup_index.end())
			{
				dup_index.insert(rs_id);
				auto &&cur_snp = m_existed_snps[m_existed_snps_index[rs_id]];
				int32_t chr_code = -1;
				if (index[+BASE_INDEX::CHR] >= 0)
				{
					chr_code = get_chrom_code_raw(token[index[+BASE_INDEX::CHR]].c_str());
					if (((const uint32_t)chr_code) > m_max_code) {
						if (chr_code != -1) {
							if (chr_code >= MAX_POSSIBLE_CHROM) {
								chr_code= m_xymt_codes[chr_code - MAX_POSSIBLE_CHROM];
							}
							else
							{
								std::string error_message ="ERROR: Cannot parse chromosome code: "
										+ token[index[+BASE_INDEX::CHR]];
								throw std::runtime_error(error_message);
							}
						}
					}
					else if(is_set(m_haploid_mask, chr_code) || chr_code==m_xymt_codes[X_OFFSET] ||
							chr_code==m_xymt_codes[Y_OFFSET])
					{
						if(!hap_error) fprintf(stderr, "WARNING: Currently not support haploid chromosome and sex chromosomes\n");
						hap_error =true;
						exclude = true;
						num_excluded++;
					}
				}
				std::string ref_allele = (index[+BASE_INDEX::REF] >= 0) ? token[index[+BASE_INDEX::REF]] : "";
				std::string alt_allele = (index[+BASE_INDEX::ALT] >= 0) ? token[index[+BASE_INDEX::ALT]] : "";
				int loc = -1;
				if (index[+BASE_INDEX::BP] >= 0)
				{
					// obtain the SNP coordinate
					try {
						loc = misc::convert<int>( token[index[+BASE_INDEX::BP]].c_str());
						if (loc < 0)
						{
							std::string error_message = "ERROR: "+rs_id+" has negative loci!\n";
							throw std::runtime_error(error_message);
						}
					} catch (const std::runtime_error &error) {
						std::string error_message = "ERROR: Non-numeric loci for "+rs_id+"!\n";
						throw std::runtime_error(error_message);
					}
				}
				bool flipped = false;
				if(!cur_snp.matching(chr_code, loc, ref_allele, alt_allele, flipped))
				{
					num_mismatched++;
					exclude = true; // hard check, as we can't tell if that is correct or not anyway
				}
				double pvalue = 2.0;
				try{
					pvalue = misc::convert<double>( token[index[+BASE_INDEX::P]]);
					if (pvalue < 0.0 || pvalue > 1.0)
					{
						std::string error_message = "ERROR: Invalid p-value for "+rs_id+"!\n";
						throw std::runtime_error(error_message);
					}
					else if (pvalue > threshold)
					{
						exclude = true;
						num_excluded++;
					}
				}catch (const std::runtime_error& error) {
					exclude = true;
					num_not_converted++;
				}
				double stat = 0.0;
				try {
					stat = misc::convert<double>( token[index[+BASE_INDEX::STAT]]);
					if(stat <0 && !beta)
					{
						num_negative_stat++;
						exclude = true;
					}
					else if (!beta) stat = log(stat);
				} catch (const std::runtime_error& error) {
					num_not_converted++;
					exclude = true;
				}


				if(!alt_allele.empty() && SNP::ambiguous(ref_allele, alt_allele)){
					num_excluded++;
					exclude= true;
				}
				if(!exclude)
				{
					int category = -1;
					double pthres = 0.0;
					if (fastscore)
					{
						category = c_commander.get_category(pvalue);
						pthres = c_commander.get_threshold(category);
					}
					else
					{
						// calculate the threshold instead
						if (pvalue > bound_end && full)
						{
							category = std::ceil((bound_end + 0.1 - bound_start) / bound_inter);
							pthres = 1.0;
						}
						else
						{
							category = std::ceil((pvalue - bound_start) / bound_inter);
							category = (category < 0) ? 0 : category;
							pthres = category * bound_inter + bound_start;
						}
					}
					if(flipped) cur_snp.set_flipped();
					// ignore the SE as it currently serves no purpose
					exist_index.push_back(m_existed_snps_index[rs_id]);
					cur_snp.set_statistic(stat, 0.0, pvalue, category, pthres);
					m_max_category = (m_max_category< category)? category:m_max_category;
				}
			}
			else if(dup_index.find(rs_id)!=dup_index.end())
			{
				num_duplicated++;
			}
			else
			{
				num_not_found++;
			}
		}
	}
	snp_file.close();
	if(exist_index.size() != m_existed_snps.size())
	{ // only do this if we need to remove some SNPs
		// we assume exist_index doesn't have any duplicated index
		std::sort(exist_index.begin(), exist_index.end());
		int start = (exist_index.empty())? -1:exist_index.front();
		int end = start;
		std::vector<SNP>::iterator last = m_existed_snps.begin();;
		for(auto && ind : exist_index)
		{
			if(ind==start||ind-end==1) end=ind; // try to perform the copy as a block
			else{
				std::copy(m_existed_snps.begin()+start, m_existed_snps.begin()+end+1,last);
				last += end+1-start;
				start =ind;
				end = ind;
			}
		}
		if(!exist_index.empty())
		{
			std::copy(m_existed_snps.begin()+start, m_existed_snps.begin()+end+1, last);
			last+= end+1-start;
		}
		m_existed_snps.erase(last, m_existed_snps.end());
	}
	m_existed_snps_index.clear();
	// now m_existed_snps is ok and can be used directly
	size_t vector_index = 0;
	for(auto &&cur_snp : m_existed_snps) // should be in the correct order
	{
		m_existed_snps_index[cur_snp.rs()] = vector_index++;
		cur_snp.set_flag( region.check(cur_snp.chr(), cur_snp.loc()));
	}
	m_region_size = region.size();

	if(num_duplicated) fprintf(stderr, "%zu duplicated variant(s) in base file\n", num_duplicated);
	if(num_excluded) fprintf(stderr, "%zu variant(s) excluded\n", num_excluded);
	if(num_not_found) fprintf(stderr, "%zu variant(s) not found in target file\n", num_not_found);
	if(num_mismatched) fprintf(stderr ,"%zu mismatched variant(s) excluded\n", num_mismatched);
	if(num_not_converted) fprintf(stderr, "%zu NA stat/p-value observed\n", num_not_converted);
	if(num_negative_stat) fprintf(stderr, "%zu negative statistic observed. Please make sure it is really OR\n", num_negative_stat);
	fprintf(stderr, "%zu total SNPs included from base file\n\n", m_existed_snps.size());
	clump_info.p_value = c_commander.clump_p();
	clump_info.r2 =  c_commander.clump_r2();
	clump_info.proxy = c_commander.proxy();
	clump_info.use_proxy = c_commander.use_proxy();
	clump_info.distance = c_commander.clump_dist();
	filter.filter_geno = c_commander.filter_geno();
	filter.filter_maf = c_commander.filter_maf();
	filter.filter_info = c_commander.filter_info();
	filter.geno = c_commander.geno();
	filter.info_score = c_commander.info_score();
	filter.maf = c_commander.maf();
}


void Genotype::clump(Genotype &reference)
{
	uintptr_t unfiltered_sample_ctv2 = QUATERCT_TO_ALIGNED_WORDCT(m_unfiltered_sample_ct);
	auto &&cur_snp = m_existed_snps.front();
	size_t bp_of_core =cur_snp.loc();
	int prev_chr= cur_snp.chr();
	int mismatch =0;
	int total = 0;
	int core_genotype_index = 0;
	int progress = 0;
	int num_snp = m_existed_snps.size();
	int snp_index =0;
	bool mismatch_error = false;
	bool require_clump = false;
	std::unordered_set<int> overlapped_snps;
	for(auto &&snp : m_existed_snps)
	{
		snp_index++;
		auto &&target = reference.m_existed_snps_index.find(snp.rs());
		if(target==reference.m_existed_snps_index.end()) continue; // only work on SNPs that are in both
		auto &&ld_snp = reference.m_existed_snps[target->second];
		total++;
		bool flipped = false; //place holder
		if(!snp.matching(ld_snp.chr(), ld_snp.loc(), ld_snp.ref(), ld_snp.alt(), flipped))
		{
			mismatch++;
			if(!mismatch_error)
			{
				fprintf(stderr, "WARNING: Mismatched SNPs between LD reference and target!\n");
				fprintf(stderr, "         Will use information from target file\n");
				fprintf(stderr, "         You should check the files are based on same genome build\n");
				mismatch_error = true;
			}
		}
		if(prev_chr!=snp.chr())
		{
			perform_clump(core_genotype_index, require_clump, snp.chr(), snp.loc());
			prev_chr = snp.chr();
		}
		else if((snp.loc()-bp_of_core) > clump_info.distance)
		{
			perform_clump(core_genotype_index, require_clump, snp.chr(), snp.loc());
		}
		// Now read in the current SNP

		clump_info.clump_index.push_back(snp_index-1); // allow us to know that target SNPs from the clump core
		overlapped_snps.insert(snp_index-1);
		uintptr_t* genotype = new uintptr_t[m_unfiltered_sample_ctl*2];
		std::memset(genotype, 0x0, m_unfiltered_sample_ctl*2*sizeof(uintptr_t));
		reference.read_genotype(genotype, snp.snp_id(), snp.file_name());

		uintptr_t ulii = m_founder_ctsplit * sizeof(intptr_t) + 2 * sizeof(int32_t) + (m_marker_ct - 1) * 2 * sizeof(double);
		uintptr_t* geno1 = new uintptr_t[3*m_founder_ctsplit +m_founder_ctv3];
		std::memset(geno1, 0x0, (3*m_founder_ctsplit +m_founder_ctv3)*sizeof(uintptr_t));
		load_and_split3(genotype, m_founder_ct, geno1, m_founder_ctv3, 0, 0, 1, &ulii);
		m_genotype.push_back(geno1);
		clump_info.missing.push_back(ulii);
		if(!require_clump && snp.p_value() < clump_info.p_value)
		{ // Set this as the core SNP
			bp_of_core =snp.loc();
			core_genotype_index=m_genotype.size()-1; // Should store the index on genotype
			require_clump= true;
		}
		fprintf(stderr, "\rClumping Progress: %03.2f%%", (double) progress++ / (double) (num_snp) * 100.0);
	}
	if(m_genotype.size()!=0)
	{
		// this make sure this will be the last
		perform_clump(core_genotype_index, require_clump, prev_chr+1, bp_of_core+2*clump_info.distance);
	}

	fprintf(stderr, "\rClumping Progress: %03.2f%%\n\n", 100.0);


	std::vector<int> remain_snps;
	m_existed_snps_index.clear();
	std::vector<size_t> p_sort_order = SNP::sort_by_p(m_existed_snps);
	bool proxy = clump_info.use_proxy;
	for(auto &&i_snp : p_sort_order){
		if(overlapped_snps.find(i_snp)!=overlapped_snps.end() &&
				m_existed_snps[i_snp].p_value() < clump_info.p_value)
		{
			if(proxy && !m_existed_snps[i_snp].clumped() )
			{
				m_existed_snps[i_snp].proxy_clump(m_existed_snps, clump_info.proxy);
				remain_snps.push_back(i_snp);
			}
			else if(!m_existed_snps[i_snp].clumped())
			{
				m_existed_snps[i_snp].clump(m_existed_snps);
				remain_snps.push_back(i_snp);
			}
		}
		else if(m_existed_snps[i_snp].p_value() >= clump_info.p_value) break;

	}

	if(remain_snps.size() != m_existed_snps.size())
	{ // only do this if we need to remove some SNPs
		// we assume exist_index doesn't have any duplicated index
		std::sort(remain_snps.begin(), remain_snps.end());
		int start = (remain_snps.empty())? -1:remain_snps.front();
		int end = start;
		std::vector<SNP>::iterator last = m_existed_snps.begin();;
		for(auto && ind : remain_snps)
		{
			if(ind==start||ind-end==1) end=ind; // try to perform the copy as a block
			else{
				std::copy(m_existed_snps.begin()+start, m_existed_snps.begin()+end+1,last);
				last += end+1-start;
				start =ind;
				end = ind;
			}
		}
		if(!remain_snps.empty())
		{
			std::copy(m_existed_snps.begin()+start, m_existed_snps.begin()+end+1, last);
			last+= end+1-start;
		}
		m_existed_snps.erase(last, m_existed_snps.end());
	}
	m_existed_snps_index.clear();
	// we don't need the index any more because we don't need to match SNPs anymore
	fprintf(stderr, "Number of SNPs after clumping : %zu\n\n", m_existed_snps.size());
}


void Genotype::perform_clump(int core_genotype_index, bool require_clump, int next_chr, int next_loc)
{
	if(m_genotype.size()==0) return; // got nothing to do
	/**
	 * DON'T MIX UP SNP INDEX AND GENOTYPE INDEX!!!
	 * SNP INDEX SHOULD ONLY BE USED FOR GETTING THE CHR, LOC AND P-VALUE
	 */
	size_t core_snp_index = clump_info.clump_index[core_genotype_index];
	int core_chr = m_existed_snps[core_snp_index].chr();
	size_t core_loc = m_existed_snps[core_snp_index].loc();

	size_t infinite_guard = 0; // guard against infinite while loop
	size_t max_possible = clump_info.clump_index.size();
	while(require_clump && (core_chr != next_chr || (next_loc - core_loc) > clump_info.distance))
	{ // as long as we still need to perform clumping
		clump_thread(core_genotype_index);
		require_clump = false;
		for(size_t core_finder = core_genotype_index+1; core_finder < clump_info.clump_index.size(); ++core_finder)
		{
			size_t run_snp_index = clump_info.clump_index[core_finder];
			if(m_existed_snps[run_snp_index].p_value() < clump_info.p_value)
			{
				core_genotype_index = core_finder; //update the core genotype index
				core_chr = m_existed_snps[run_snp_index].chr();
				core_loc = m_existed_snps[run_snp_index].loc();
				require_clump= true;
				break;
			}
		}
		// New core found, need to clean things up a bit
		if(require_clump)
		{
			size_t num_remove = 0;
			for(size_t remover = 0; remover < core_genotype_index; ++remover)
			{
				size_t run_snp_index =clump_info.clump_index[remover];
				if(core_loc-m_existed_snps[run_snp_index].loc() > clump_info.distance) num_remove++;
				else break;
			}
			if(num_remove!=0)
			{
				lerase(num_remove);
				core_genotype_index-=num_remove; // only update the index when we remove stuff
			}
		}
		infinite_guard++;
		if(infinite_guard>max_possible) throw std::logic_error("ERROR: While loop run longer than expected");
	}
	// for this to be true, the require clump should be false or the core_snp is now within reach of the
	// new snp
	if(core_chr!= next_chr)
	{ // next_chr is not within m_genotype and clump_index yet
		lerase(m_genotype.size());
	}
	else if(!require_clump)
	{
		//remove anything that is too far ahead
		size_t num_remove = 0;

		for(auto &&remover : clump_info.clump_index)
		{
			if(next_loc-m_existed_snps[remover].loc()>clump_info.distance) num_remove++;
			else break;
		}
		if(num_remove!=0)
		{
			lerase(num_remove);
		}
	}
}

void Genotype::clump_thread(const size_t c_core_genotype_index)
{
	size_t wind_size = clump_info.clump_index.size();
	if(wind_size <=1 ) return; // nothing to do
	uint32_t tot1[6];
	bool nm_fixed = false;
	tot1[0] = popcount_longs(m_genotype[c_core_genotype_index], m_founder_ctv3);
	tot1[1] = popcount_longs(&(m_genotype[c_core_genotype_index][m_founder_ctv3]), m_founder_ctv3);
	tot1[2] = popcount_longs(&(m_genotype[c_core_genotype_index][2 * m_founder_ctv3]), m_founder_ctv3);
	// in theory, std::fill, std::copy should be safer than these mem thing
	// but as a safety guard from my stupidity, let's just follow plink
	/*
		if (is_x) {
			memcpy(geno_male, geno1, founder_ctsplit * sizeof(intptr_t));
			bitvec_and(m_sex_male, founder_ctv3, geno_male);
			tot1[3] = popcount_longs(geno_male, founder_ctv3);
			bitvec_and(m_sex_male, founder_ctv3, &(geno_male[founder_ctv3]));
			tot1[4] = popcount_longs(&(geno_male[founder_ctv3]), founder_ctv3);
			bitvec_and(m_sex_male, founder_ctv3, &(geno_male[2 * founder_ctv3]));
			tot1[5] = popcount_longs(&(geno_male[2 * founder_ctv3]), founder_ctv3);
		}
	 */
	if (clump_info.missing[c_core_genotype_index] == 3)
	{
		nm_fixed = true;
	}

	std::vector<std::thread> thread_store;
	if((wind_size-1) < m_thread)
	{
		for(size_t i_snp = 0; i_snp < wind_size; ++i_snp)
		{
			if(i_snp==c_core_genotype_index) continue;

			thread_store.push_back(std::thread(&Genotype::compute_clump, this,
					c_core_genotype_index,i_snp, i_snp+1, nm_fixed,
					std::ref(tot1)));

		}
	}
	else
	{
		int num_snp_per_thread =(int)(wind_size) / (int)m_thread;  //round down
		int remain = (int)(wind_size) % (int)m_thread;
		int cur_start = 0;
		int cur_end = num_snp_per_thread;
		for(size_t i_thread = 0; i_thread < m_thread; ++i_thread)
		{

			thread_store.push_back(std::thread(&Genotype::compute_clump, this, c_core_genotype_index,
					cur_start, cur_end+(remain>0), nm_fixed, std::ref(tot1)));

			cur_start = cur_end+(remain>0);
			cur_end+=num_snp_per_thread+(remain>0);
			if(cur_end>wind_size) cur_end =wind_size;
			remain--;
		}
	}
	for(auto &&thread_runner : thread_store) thread_runner.join();
	thread_store.clear();

	//delete [] geno_male;
}

void Genotype::compute_clump( size_t core_genotype_index, size_t i_start, size_t i_end, bool nm_fixed, uint32_t* tot1)
{
	uintptr_t* loadbuf;
	uint32_t marker_idx2_maxw =  m_marker_ct - 1;
	uint32_t counts[18];
	double freq11;
	double freq11_expected;
	double freq1x;
	double freq2x;
	double freqx1;
	double freqx2;
	double dxx;
	double r2 =0.0;
	bool zmiss2=false;
	size_t max_size = clump_info.clump_index.size();
	size_t ref_snp_index = clump_info.clump_index[core_genotype_index];
	double ref_p_value = m_existed_snps.at(ref_snp_index).p_value();
	std::vector<double> r2_store;
	std::vector<size_t> target_index_store; // index we want to push into the current index

	for(size_t i_snp = i_start; i_snp < i_end && i_snp < max_size; ++i_snp)
	{
		zmiss2 = false;
		size_t target_snp_index = clump_info.clump_index[i_snp];
		if(i_snp != core_genotype_index && (
				(m_existed_snps[target_snp_index].p_value() > ref_p_value)||
				(m_existed_snps[target_snp_index].p_value()==ref_p_value &&
						(m_existed_snps[target_snp_index].loc() > m_existed_snps[ref_snp_index].loc())
				)
		))
		{
			// if the target is not as significant as the reference SNP or if it is the same significance but with
			// appear later in the genome


			uintptr_t uiptr[3];
			uiptr[0] = popcount_longs(m_genotype[i_snp], m_founder_ctv3);
			uiptr[1] = popcount_longs(&(m_genotype[i_snp][m_founder_ctv3]), m_founder_ctv3);
			uiptr[2] = popcount_longs(&(m_genotype[i_snp][2 * m_founder_ctv3]), m_founder_ctv3);
			if (clump_info.missing[i_snp] == 3) {
				zmiss2 = true;
			}

			/*
			if (nm_fixed) {
				two_locus_count_table_zmiss1(m_genotype[core_genotype_index], m_genotype[i_snp],
						counts, m_founder_ctv3, zmiss2);
				if (zmiss2) {
					counts[2] = tot1[0] - counts[0] - counts[1];
					counts[5] = tot1[1] - counts[3] - counts[4];
				}
				counts[6] = uiptr[0] - counts[0] - counts[3];
            		counts[7] = uiptr[1] - counts[1] - counts[4];
            		counts[8] = uiptr[2] - counts[2] - counts[5];
			} else {
				two_locus_count_table(m_genotype[core_genotype_index], m_genotype[i_snp],
						counts, m_founder_ctv3, zmiss2);
				if (zmiss2) {
					counts[2] = tot1[0] - counts[0] - counts[1];
					counts[5] = tot1[1] - counts[3] - counts[4];
					counts[8] = tot1[2] - counts[6] - counts[7];
				}
			}
		*/
			/*
	    		// good thing is that the x1 and x2 must always be the same
	    		if (is_x1 || is_x2) {
	    			two_locus_count_table(geno_male, ulptr, &(counts[9]), founder_ctv3, zmiss2);
	    			if (zmiss2) {
	    				counts[11] = tot1[3] - counts[9] - counts[10];
	    				counts[14] = tot1[4] - counts[12] - counts[13];
	    				counts[17] = tot1[5] - counts[15] - counts[16];
	    			}
	    		}
			 */
			// below, the false are basically is_x1 is_x2
			/*
			if(em_phase_hethet_nobase(counts, false, false, &freq1x, &freq2x, &freqx1, &freqx2, &freq11))
			{
				r2 = -1;
			}
			else
			{
				freq11_expected = freqx1 * freq1x;
				dxx = freq11 - freq11_expected;
				if (fabs(dxx) < SMALL_EPSILON)
				{
					r2 = 0.0;
				}
				else
				{
					r2 = dxx * dxx / (freq11_expected * freq2x * freqx2);
				}
			}
			*/
			if(r2 >= clump_info.r2)
			{
				target_index_store.push_back(target_snp_index);
				r2_store.push_back(r2);
			}
		}
	}


	/*
	Genotype::clump_mtx.lock();
	m_existed_snps[ref_snp_index].add_clump(target_index_store);
	m_existed_snps[ref_snp_index].add_clump_r2(r2_store);
	Genotype::clump_mtx.unlock();
	*/
}

void Genotype::lerase(int num)
{
	if(num <0)
	{
		std::string error_message = "Number of removed SNPs cannot be less than 1: "+std::to_string(num);
		throw std::runtime_error(error_message);
	}
	if(num > m_genotype.size())
	{
		std::string error_message = "Number of removed SNPs exceed number of SNPs available "+std::to_string(num)+" "+std::to_string(m_genotype.size());
		throw std::runtime_error(error_message);
	}
	for(size_t i = 0; i < num; ++i)
	{
		delete [] m_genotype[i];
	}
	if(num==m_genotype.size())
	{
		m_genotype.clear();
		clump_info.clump_index.clear();
		clump_info.missing.clear();
	}
	else
	{
		m_genotype.erase(m_genotype.begin(), m_genotype.begin()+num);
		clump_info.clump_index.erase(clump_info.clump_index.begin(), clump_info.clump_index.begin()+num);
		clump_info.missing.erase(clump_info.missing.begin(), clump_info.missing.begin()+num);
	}

}

bool Genotype::prepare_prsice()
{
	if(m_existed_snps.size()==0) return false;
	std::sort(begin(m_existed_snps), end(m_existed_snps),
			[](SNP const &t1, SNP const &t2)
	      {
	        if(t1.category()==t2.category())
	        {
	          if(t1.file_name().compare(t2.file_name())==0)
	          {
	            return t1.snp_id()<t2.snp_id();
	          }
	          else return t1.file_name().compare(t2.file_name())<0;
	        }
	        else return t1.category()<t2.category();
	      });
	return true;
}

bool Genotype::get_score(std::vector< std::vector<Sample_lite> > &prs_score, int &cur_index, int &cur_category,
		double &cur_threshold, std::vector<size_t> &num_snp_included)
{
	if(m_existed_snps.size() ==0) return false;
	int end_index = 0;
	bool ended = false;
	cur_threshold = m_existed_snps[cur_index].get_threshold();
	for (size_t i = cur_index; i < m_existed_snps.size(); ++i)
	{
		if (m_existed_snps[i].category() != cur_index)
		{
			end_index = i;
			ended = true;
			break;
		}
//		// Use as part of the output
		for (size_t i_region = 0; i_region < m_region_size; ++i_region)
		{
			if (m_existed_snps[i].in( i_region)) num_snp_included[i_region]++;
		}
	}
	if (!ended) end_index = m_existed_snps.size();
	//get_score(m_partition, m_snp_list, m_current_prs, cur_index, end_index, m_region_size, m_scoring);

	read_score(prs_score, cur_index, end_index);
	cur_index = end_index;
	return true;
}

/**
 * DON'T TOUCH AREA
 *
 */

double Genotype::calc_lnlike(double known11, double known12, double known21, double known22, double center_ct_d,
		double freq11, double freq12, double freq21, double freq22, double half_hethet_share, double freq11_incr) {
	double lnlike;
	freq11 += freq11_incr;
	freq22 += freq11_incr;
	freq12 += half_hethet_share - freq11_incr;
	freq21 += half_hethet_share - freq11_incr;
	lnlike = center_ct_d * log(freq11 * freq22 + freq12 * freq21);
	if (known11 != 0.0) {
		lnlike += known11 * log(freq11);
	}
	if (known12 != 0.0) {
		lnlike += known12 * log(freq12);
	}
	if (known21 != 0.0) {
		lnlike += known21 * log(freq21);
	}
	if (known22 != 0.0) {
		lnlike += known22 * log(freq22);
	}
	return lnlike;
}

// This is where the magic happens
uint32_t Genotype::em_phase_hethet(double known11, double known12, double known21, double known22, uint32_t center_ct,
		double* freq1x_ptr, double* freq2x_ptr, double* freqx1_ptr, double* freqx2_ptr, double* freq11_ptr,
		uint32_t* onside_sol_ct_ptr) {
	// Returns 1 if at least one SNP is monomorphic over all valid observations;
	// returns 0 otherwise, and fills all frequencies using the maximum
	// likelihood solution to the cubic equation.
	// (We're discontinuing most use of EM phasing since better algorithms have
	// been developed, but the two marker case is mathematically clean and fast
	// enough that it'll probably remain useful as an input for some of those
	// better algorithms...)
	double center_ct_d = (int32_t)center_ct;
	double twice_tot = known11 + known12 + known21 + known22 + 2 * center_ct_d;
	uint32_t sol_start_idx = 0;
	uint32_t sol_end_idx = 1;
	double solutions[3];
	double twice_tot_recip;
	double half_hethet_share;
	double freq11;
	double freq12;
	double freq21;
	double freq22;
	double prod_1122;
	double prod_1221;
	double incr_1122;
	double best_sol;
	double best_lnlike;
	double cur_lnlike;
	double freq1x;
	double freq2x;
	double freqx1;
	double freqx2;
	double lbound;
	double dxx;
	uint32_t cur_sol_idx;
	// shouldn't have to worry about subtractive cancellation problems here
	if (twice_tot == 0.0) {
		return 1;
	}
	twice_tot_recip = 1.0 / twice_tot;
	freq11 = known11 * twice_tot_recip;
	freq12 = known12 * twice_tot_recip;
	freq21 = known21 * twice_tot_recip;
	freq22 = known22 * twice_tot_recip;
	prod_1122 = freq11 * freq22;
	prod_1221 = freq12 * freq21;
	half_hethet_share = center_ct_d * twice_tot_recip;
	// the following four values should all be guaranteed nonzero except in the
	// NAN case
	freq1x = freq11 + freq12 + half_hethet_share;
	freq2x = 1.0 - freq1x;
	freqx1 = freq11 + freq21 + half_hethet_share;
	freqx2 = 1.0 - freqx1;
	if (center_ct) {
		if ((prod_1122 != 0.0) || (prod_1221 != 0.0)) {
			sol_end_idx = cubic_real_roots(0.5 * (freq11 + freq22 - freq12 - freq21 - 3 * half_hethet_share),
					0.5 * (prod_1122 + prod_1221 + half_hethet_share * (freq12 + freq21 - freq11 - freq22 + half_hethet_share)),
					-0.5 * half_hethet_share * prod_1122, solutions);
			while (sol_end_idx && (solutions[sol_end_idx - 1] > half_hethet_share + SMALLISH_EPSILON))
			{
				sol_end_idx--;
				//assert(sol_end_idx && sol_end_idx-1 >= 0);
			}
			while ((sol_start_idx < sol_end_idx) && (solutions[sol_start_idx] < -SMALLISH_EPSILON))
			{
				sol_start_idx++;
				//assert((sol_start_idx < sol_end_idx) &&sol_start_idx < 3);
			}
			if (sol_start_idx == sol_end_idx)
			{
				// Lost a planet Master Obi-Wan has.  How embarrassing...
				// lost root must be a double root at one of the boundary points, just
				// check their likelihoods
				sol_start_idx = 0;
				sol_end_idx = 2;
				solutions[0] = 0;
				solutions[1] = half_hethet_share;
			}
			else
			{
				if (solutions[sol_start_idx] < 0)
				{
					solutions[sol_start_idx] = 0;
				}
				// checking here
				if (solutions[sol_end_idx-1] > half_hethet_share)
				{
					solutions[sol_end_idx-1] = half_hethet_share;
				}
			}
		} else {
			solutions[0] = 0;
			if ((freq22 + SMALLISH_EPSILON < half_hethet_share + freq21) && (freq21 + SMALLISH_EPSILON < half_hethet_share + freq22)) {
				sol_end_idx = 3;
				solutions[1] = (half_hethet_share + freq21 - freq22) * 0.5;
				solutions[2] = half_hethet_share;
			} else {
				sol_end_idx = 2;
				solutions[1] = half_hethet_share;
			}
		}
		best_sol = solutions[sol_start_idx];
		if (sol_end_idx > sol_start_idx + 1) {
			// select largest log likelihood
			best_lnlike = calc_lnlike(known11, known12, known21, known22, center_ct_d, freq11, freq12, freq21, freq22, half_hethet_share, best_sol);
			cur_sol_idx = sol_start_idx + 1;
			do {
				incr_1122 = solutions[cur_sol_idx];
				cur_lnlike = calc_lnlike(known11, known12, known21, known22, center_ct_d, freq11, freq12, freq21, freq22, half_hethet_share, incr_1122);
				if (cur_lnlike > best_lnlike) {
					cur_lnlike = best_lnlike;
					best_sol = incr_1122;
				}
			} while (++cur_sol_idx < sol_end_idx);
		}
		if (onside_sol_ct_ptr && (sol_end_idx > sol_start_idx + 1)) {
			if (freqx1 * freq1x >= freq11) {
				dxx = freq1x * freqx1 - freq11;
				if (dxx > half_hethet_share) {
					dxx = half_hethet_share;
				}
			} else {
				dxx = 0.0;
			}
			// okay to NOT count suboptimal boundary points because they don't permit
			// direction changes within the main interval
			// this should exactly match haploview_blocks_classify()'s D sign check
			if ((freq11 + best_sol) - freqx1 * freq1x >= 0.0) {
				if (best_sol > dxx + SMALLISH_EPSILON) {
					lbound = dxx + SMALLISH_EPSILON;
				} else {
					lbound = dxx;
				}
				if (best_sol < half_hethet_share - SMALLISH_EPSILON) {
					half_hethet_share -= SMALLISH_EPSILON;
				}
			} else {
				if (best_sol > SMALLISH_EPSILON) {
					lbound = SMALLISH_EPSILON;
				} else {
					lbound = 0.0;
				}
				if (best_sol < dxx - SMALLISH_EPSILON) {
					half_hethet_share = dxx - SMALLISH_EPSILON;
				} else {
					half_hethet_share = dxx;
				}
			}
			for (cur_sol_idx = sol_start_idx; cur_sol_idx < sol_end_idx; cur_sol_idx++) {
				if (solutions[cur_sol_idx] < lbound) {
					sol_start_idx++;
				}
				if (solutions[cur_sol_idx] > half_hethet_share) {
					break;
				}
			}
			if (cur_sol_idx >= sol_start_idx + 2) {
				*onside_sol_ct_ptr = cur_sol_idx - sol_start_idx;
			}
		}
		freq11 += best_sol;
	} else if ((prod_1122 == 0.0) && (prod_1221 == 0.0)) {
		return 1;
	}
	*freq1x_ptr = freq1x;
	*freq2x_ptr = freq2x;
	*freqx1_ptr = freqx1;
	*freqx2_ptr = freqx2;
	*freq11_ptr = freq11;
	return 0;
}

uint32_t Genotype::em_phase_hethet_nobase(uint32_t* counts, uint32_t is_x1, uint32_t is_x2, double* freq1x_ptr,
		double* freq2x_ptr, double* freqx1_ptr, double* freqx2_ptr, double* freq11_ptr) {
	// if is_x1 and/or is_x2 is set, counts[9]..[17] are male-only counts.
	double known11 = (double)(2 * counts[0] + counts[1] + counts[3]);
	double known12 = (double)(2 * counts[2] + counts[1] + counts[5]);
	double known21 = (double)(2 * counts[6] + counts[3] + counts[7]);
	double known22 = (double)(2 * counts[8] + counts[5] + counts[7]);
	if (is_x1 || is_x2) {
		if (is_x1 && is_x2) {
			known11 -= (double)((int32_t)counts[9]);
			known12 -= (double)((int32_t)counts[11]);
			known21 -= (double)((int32_t)counts[15]);
			known22 -= (double)((int32_t)counts[17]);
		} else if (is_x1) {
			known11 -= ((double)(2 * counts[9] + counts[10])) * (1.0 - SQRT_HALF);
			known12 -= ((double)(2 * counts[11] + counts[10])) * (1.0 - SQRT_HALF);
			known21 -= ((double)(2 * counts[15] + counts[16])) * (1.0 - SQRT_HALF);
			known22 -= ((double)(2 * counts[17] + counts[16])) * (1.0 - SQRT_HALF);
		} else {
			known11 -= ((double)(2 * counts[9] + counts[12])) * (1.0 - SQRT_HALF);
			known12 -= ((double)(2 * counts[11] + counts[12])) * (1.0 - SQRT_HALF);
			known21 -= ((double)(2 * counts[15] + counts[14])) * (1.0 - SQRT_HALF);
			known22 -= ((double)(2 * counts[17] + counts[14])) * (1.0 - SQRT_HALF);
		}
	}
	return em_phase_hethet(known11, known12, known21, known22, counts[4], freq1x_ptr, freq2x_ptr, freqx1_ptr, freqx2_ptr, freq11_ptr, nullptr);
}


uint32_t Genotype::load_and_split3(uintptr_t* rawbuf, uint32_t unfiltered_sample_ct, uintptr_t* casebuf, uint32_t case_ctv, uint32_t ctrl_ctv, uint32_t do_reverse, uint32_t is_case_only, uintptr_t* nm_info_ptr)
{
	uintptr_t* rawbuf_end = &(rawbuf[unfiltered_sample_ct / BITCT2]);
	uintptr_t* ctrlbuf = &(casebuf[3 * case_ctv]);
	uintptr_t case_words[4];
	uintptr_t ctrl_words[4];
	uint32_t unfiltered_sample_ct4 = (unfiltered_sample_ct + 3) / 4;
	uint32_t case_rem = 0;
	uint32_t ctrl_rem = 0;
	uint32_t read_shift_max = BITCT2;
	uint32_t sample_uidx = 0;
	uint32_t offset0_case = do_reverse * 2 * case_ctv;
	uint32_t offset2_case = (1 - do_reverse) * 2 * case_ctv;
	uint32_t offset0_ctrl = do_reverse * 2 * ctrl_ctv;
	uint32_t offset2_ctrl = (1 - do_reverse) * 2 * ctrl_ctv;
	uint32_t read_shift;
	uintptr_t read_word;
	uintptr_t ulii;

	case_words[0] = 0;
	case_words[1] = 0;
	case_words[2] = 0;
	case_words[3] = 0;
	ctrl_words[0] = 0;
	ctrl_words[1] = 0;
	ctrl_words[2] = 0;
	ctrl_words[3] = 0;
	while (1) {
		while (rawbuf < rawbuf_end) {
			read_word = *rawbuf++;
			for (read_shift = 0; read_shift < read_shift_max; sample_uidx++, read_shift++) {
				ulii = read_word & 3; // Both is_set is always true, because dummy_nm is set
				case_words[ulii] |= ONELU << case_rem;
				if (++case_rem == BITCT) {
					casebuf[offset0_case] = case_words[0];
					casebuf[case_ctv] = case_words[2];
					casebuf[offset2_case] = case_words[3];
					casebuf++;
					case_words[0] = 0;
					case_words[2] = 0;
					case_words[3] = 0;
					case_rem = 0;
				}
				read_word >>= 2;
			}
		}
		if (sample_uidx == unfiltered_sample_ct) {
			if (case_rem) {
				casebuf[offset0_case] = case_words[0];
				casebuf[case_ctv] = case_words[2];
				casebuf[offset2_case] = case_words[3];
			}
			if (ctrl_rem) {
				ctrlbuf[offset0_ctrl] = ctrl_words[0];
				ctrlbuf[ctrl_ctv] = ctrl_words[2];
				ctrlbuf[offset2_ctrl] = ctrl_words[3];
			}
			ulii = 3;
			if (case_words[1]) {
				ulii -= 1;
			}
			if (ctrl_words[1]) {
				ulii -= 2;
			}
			*nm_info_ptr = ulii;
			return 0;
		}
		rawbuf_end++;
		read_shift_max = unfiltered_sample_ct % BITCT2;
  }
}



void Genotype::two_locus_count_table(uintptr_t* lptr1, uintptr_t* lptr2, uint32_t* counts_3x3, uint32_t sample_ctv3,
		uint32_t is_zmiss2) {
#ifdef __LP64__
  uint32_t uii;
  fill_uint_zero(9, counts_3x3);
  if (!is_zmiss2) {
    two_locus_3x3_tablev((__m128i*)lptr1, (__m128i*)lptr2, counts_3x3, sample_ctv3 / 2, 3);
  } else {
    two_locus_3x3_tablev((__m128i*)lptr2, (__m128i*)lptr1, counts_3x3, sample_ctv3 / 2, 2);
    uii = counts_3x3[1];
    counts_3x3[1] = counts_3x3[3];
    counts_3x3[3] = uii;
    counts_3x3[6] = counts_3x3[2];
    counts_3x3[7] = counts_3x3[5];
  }
#else
  counts_3x3[0] = popcount_longs_intersect(lptr2, lptr1, sample_ctv3);
  counts_3x3[3] = popcount_longs_intersect(lptr2, &(lptr1[sample_ctv3]), sample_ctv3);
  counts_3x3[6] = popcount_longs_intersect(lptr2, &(lptr1[2 * sample_ctv3]), sample_ctv3);
  lptr2 = &(lptr2[sample_ctv3]);
  counts_3x3[1] = popcount_longs_intersect(lptr2, lptr1, sample_ctv3);
  counts_3x3[4] = popcount_longs_intersect(lptr2, &(lptr1[sample_ctv3]), sample_ctv3);
  counts_3x3[7] = popcount_longs_intersect(lptr2, &(lptr1[2 * sample_ctv3]), sample_ctv3);
  if (!is_zmiss2) {
    lptr2 = &(lptr2[sample_ctv3]);
    counts_3x3[2] = popcount_longs_intersect(lptr2, lptr1, sample_ctv3);
    counts_3x3[5] = popcount_longs_intersect(lptr2, &(lptr1[sample_ctv3]), sample_ctv3);
    counts_3x3[8] = popcount_longs_intersect(lptr2, &(lptr1[2 * sample_ctv3]), sample_ctv3);
  }
#endif
}

void Genotype::two_locus_count_table_zmiss1(uintptr_t* lptr1, uintptr_t* lptr2, uint32_t* counts_3x3,
		uint32_t sample_ctv3, uint32_t is_zmiss2) {

#ifdef __LP64__
  fill_uint_zero(6, counts_3x3);
  if (is_zmiss2) {
    two_locus_3x3_zmiss_tablev((__m128i*)lptr1, (__m128i*)lptr2, counts_3x3, sample_ctv3 / 2);
  } else {
    two_locus_3x3_tablev((__m128i*)lptr1, (__m128i*)lptr2, counts_3x3, sample_ctv3 / 2, 2);
  }
#else
  counts_3x3[0] = popcount_longs_intersect(lptr1, lptr2, sample_ctv3);
  counts_3x3[1] = popcount_longs_intersect(lptr1, &(lptr2[sample_ctv3]), sample_ctv3);
  if (!is_zmiss2) {
    counts_3x3[2] = popcount_longs_intersect(lptr1, &(lptr2[2 * sample_ctv3]), sample_ctv3);
    counts_3x3[5] = popcount_longs_intersect(&(lptr1[sample_ctv3]), &(lptr2[2 * sample_ctv3]), sample_ctv3);
  }
  lptr1 = &(lptr1[sample_ctv3]);
  counts_3x3[3] = popcount_longs_intersect(lptr1, lptr2, sample_ctv3);
  counts_3x3[4] = popcount_longs_intersect(lptr1, &(lptr2[sample_ctv3]), sample_ctv3);
#endif
}


#ifdef __LP64__
void Genotype::two_locus_3x3_tablev(__m128i* vec1, __m128i* vec2, uint32_t* counts_3x3, uint32_t sample_ctv6,
		uint32_t iter_ct) {
  const __m128i m1 = {FIVEMASK, FIVEMASK};
  const __m128i m2 = {0x3333333333333333LLU, 0x3333333333333333LLU};
  const __m128i m4 = {0x0f0f0f0f0f0f0f0fLLU, 0x0f0f0f0f0f0f0f0fLLU};
  __m128i* vec20;
  __m128i* vec21;
  __m128i* vec22;
  __m128i* vend1;
  __m128i loader1;
  __m128i loader20;
  __m128i loader21;
  __m128i loader22;
  __m128i count10;
  __m128i count11;
  __m128i count12;
  __m128i count20;
  __m128i count21;
  __m128i count22;
  __univec acc0;
  __univec acc1;
  __univec acc2;
  uint32_t ct;
  uint32_t ct2;
  while (iter_ct--) {
    ct = sample_ctv6;
    vec20 = vec2;
    vec21 = &(vec20[sample_ctv6]);
    vec22 = &(vec20[2 * sample_ctv6]);
    while (ct >= 30) {
      ct -= 30;
      vend1 = &(vec1[30]);
      acc0.vi = _mm_setzero_si128();
      acc1.vi = _mm_setzero_si128();
      acc2.vi = _mm_setzero_si128();
      do {
      two_locus_3x3_tablev_outer:
	loader1 = *vec1++;
	loader20 = *vec20++;
	loader21 = *vec21++;
	loader22 = *vec22++;
	count10 = _mm_and_si128(loader1, loader20);
	count11 = _mm_and_si128(loader1, loader21);
	count12 = _mm_and_si128(loader1, loader22);
	count10 = _mm_sub_epi64(count10, _mm_and_si128(_mm_srli_epi64(count10, 1), m1));
	count11 = _mm_sub_epi64(count11, _mm_and_si128(_mm_srli_epi64(count11, 1), m1));
	count12 = _mm_sub_epi64(count12, _mm_and_si128(_mm_srli_epi64(count12, 1), m1));
      two_locus_3x3_tablev_two_left:
        // unlike the zmiss variant, this apparently does not suffer from
	// enough register spill to justify shrinking the inner loop
	loader1 = *vec1++;
	loader20 = *vec20++;
	loader21 = *vec21++;
	loader22 = *vec22++;
	count20 = _mm_and_si128(loader1, loader20);
	count21 = _mm_and_si128(loader1, loader21);
	count22 = _mm_and_si128(loader1, loader22);
	count20 = _mm_sub_epi64(count20, _mm_and_si128(_mm_srli_epi64(count20, 1), m1));
	count21 = _mm_sub_epi64(count21, _mm_and_si128(_mm_srli_epi64(count21, 1), m1));
	count22 = _mm_sub_epi64(count22, _mm_and_si128(_mm_srli_epi64(count22, 1), m1));
      two_locus_3x3_tablev_one_left:
	loader1 = *vec1++;
	loader20 = *vec20++;
	loader21 = _mm_and_si128(loader1, loader20); // half1
	loader22 = _mm_and_si128(_mm_srli_epi64(loader21, 1), m1); // half2
	count10 = _mm_add_epi64(count10, _mm_and_si128(loader21, m1));
	count20 = _mm_add_epi64(count20, loader22);
	loader20 = *vec21++;
	loader21 = _mm_and_si128(loader1, loader20);
	loader22 = _mm_and_si128(_mm_srli_epi64(loader21, 1), m1);
	count11 = _mm_add_epi64(count11, _mm_and_si128(loader21, m1));
	count21 = _mm_add_epi64(count21, loader22);
	loader20 = *vec22++;
	loader21 = _mm_and_si128(loader1, loader20);
	loader22 = _mm_and_si128(_mm_srli_epi64(loader21, 1), m1);
	count12 = _mm_add_epi64(count12, _mm_and_si128(loader21, m1));
	count22 = _mm_add_epi64(count22, loader22);

	count10 = _mm_add_epi64(_mm_and_si128(count10, m2), _mm_and_si128(_mm_srli_epi64(count10, 2), m2));
	count11 = _mm_add_epi64(_mm_and_si128(count11, m2), _mm_and_si128(_mm_srli_epi64(count11, 2), m2));
	count12 = _mm_add_epi64(_mm_and_si128(count12, m2), _mm_and_si128(_mm_srli_epi64(count12, 2), m2));
	count10 = _mm_add_epi64(count10, _mm_add_epi64(_mm_and_si128(count20, m2), _mm_and_si128(_mm_srli_epi64(count20, 2), m2)));
	count11 = _mm_add_epi64(count11, _mm_add_epi64(_mm_and_si128(count21, m2), _mm_and_si128(_mm_srli_epi64(count21, 2), m2)));
	count12 = _mm_add_epi64(count12, _mm_add_epi64(_mm_and_si128(count22, m2), _mm_and_si128(_mm_srli_epi64(count22, 2), m2)));
	acc0.vi = _mm_add_epi64(acc0.vi, _mm_add_epi64(_mm_and_si128(count10, m4), _mm_and_si128(_mm_srli_epi64(count10, 4), m4)));
	acc1.vi = _mm_add_epi64(acc1.vi, _mm_add_epi64(_mm_and_si128(count11, m4), _mm_and_si128(_mm_srli_epi64(count11, 4), m4)));
	acc2.vi = _mm_add_epi64(acc2.vi, _mm_add_epi64(_mm_and_si128(count12, m4), _mm_and_si128(_mm_srli_epi64(count12, 4), m4)));
      } while (vec1 < vend1);
      const __m128i m8 = {0x00ff00ff00ff00ffLLU, 0x00ff00ff00ff00ffLLU};
      acc0.vi = _mm_add_epi64(_mm_and_si128(acc0.vi, m8), _mm_and_si128(_mm_srli_epi64(acc0.vi, 8), m8));
      acc1.vi = _mm_add_epi64(_mm_and_si128(acc1.vi, m8), _mm_and_si128(_mm_srli_epi64(acc1.vi, 8), m8));
      acc2.vi = _mm_add_epi64(_mm_and_si128(acc2.vi, m8), _mm_and_si128(_mm_srli_epi64(acc2.vi, 8), m8));
      counts_3x3[0] += ((acc0.u8[0] + acc0.u8[1]) * 0x1000100010001LLU) >> 48;
      counts_3x3[1] += ((acc1.u8[0] + acc1.u8[1]) * 0x1000100010001LLU) >> 48;
      counts_3x3[2] += ((acc2.u8[0] + acc2.u8[1]) * 0x1000100010001LLU) >> 48;
    }
    if (ct) {
      vend1 = &(vec1[ct]);
      ct2 = ct % 3;
      acc0.vi = _mm_setzero_si128();
      acc1.vi = _mm_setzero_si128();
      acc2.vi = _mm_setzero_si128();
      ct = 0;
      if (ct2) {
	count10 = _mm_setzero_si128();
	count11 = _mm_setzero_si128();
	count12 = _mm_setzero_si128();
	if (ct2 == 2) {
	  goto two_locus_3x3_tablev_two_left;
	}
	count20 = _mm_setzero_si128();
	count21 = _mm_setzero_si128();
	count22 = _mm_setzero_si128();
	goto two_locus_3x3_tablev_one_left;
      }
      goto two_locus_3x3_tablev_outer;
    }
    counts_3x3 = &(counts_3x3[3]);
  }
}
#endif