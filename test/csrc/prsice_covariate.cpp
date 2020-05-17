#include "catch.hpp"
#include "mock_genotype.hpp"
#include "mock_prsice.hpp"

TEST_CASE("Check if covariate is valid")
{
    std::vector<std::string> covariates = {"ID1", "ID1", "1",     "1", "str",
                                           "1",   "nan", "TypeA", "1", "NA",
                                           "1",   "1",   "EUR"};
    std::vector<size_t> missing_count;
    SECTION("All valid")
    {
        std::vector<size_t> cov_idx = {2, 3, 5, 7, 11, 12};
        std::set<size_t> factor_idx = {7, 12};
        missing_count.resize(cov_idx.size(), 0);
        REQUIRE(mock_prsice::test_is_valid_covariate(
            factor_idx, cov_idx, covariates, missing_count));
        REQUIRE_THAT(missing_count, Catch::Equals<size_t>(std::vector<size_t>(
                                        cov_idx.size(), 0)));
    }
    SECTION("With missing or invalid")
    {
        auto invalid_idx = GENERATE(4ul, 6ul, 9ul);
        std::vector<size_t> cov_idx = {2, 3, 5, 7, 11, 12, invalid_idx};
        std::sort(cov_idx.begin(), cov_idx.end());
        missing_count.resize(cov_idx.size(), 0);
        std::set<size_t> factor_idx = {7, 12};
        REQUIRE_FALSE(mock_prsice::test_is_valid_covariate(
            factor_idx, cov_idx, covariates, missing_count));
        std::vector<size_t> expected(cov_idx.size(), 0);
        for (size_t i = 0; i < expected.size(); ++i)
        {
            if (cov_idx[i] == invalid_idx) { ++expected[i]; }
        }
        REQUIRE_THAT(missing_count, Catch::Equals<size_t>(expected));
    }
    SECTION("With multiple problem")
    {
        std::vector<size_t> cov_idx = {2, 3, 4, 5, 6, 7, 11, 12};
        std::set<size_t> factor_idx = {7, 12};
        missing_count.resize(cov_idx.size(), 0);
        REQUIRE_FALSE(mock_prsice::test_is_valid_covariate(
            factor_idx, cov_idx, covariates, missing_count));
        std::vector<size_t> expected(cov_idx.size(), 0);
        expected[2] = 1;
        expected[4] = 1;
        REQUIRE_THAT(missing_count, Catch::Equals<size_t>(expected));
    }
}

TEST_CASE("covarience check and factor level count")
{
    // check detect duplicated ID (shouldn't check if no valid pheno)
    // check resulting levels
    // proper termination if no valid samples
    // need to initialize m_sample_with_phenotypes
    const size_t num_sample = 1000;
    std::random_device rnd_device;
    std::mt19937 mersenne_engine {rnd_device()};
    // select ~70% of samples
    std::uniform_int_distribution<size_t> dist {1, 10};
    std::uniform_int_distribution<size_t> sex_dist {0, 1};
    std::uniform_int_distribution<size_t> batch_dist {0, 100};
    auto valid = [&dist, &mersenne_engine]() {
        return dist(mersenne_engine) > 7;
    };
    auto batch = [&batch_dist, &mersenne_engine] {
        return "b" + std::to_string(batch_dist(mersenne_engine));
    };
    auto sex = [&sex_dist, &mersenne_engine] {
        switch (sex_dist(mersenne_engine))
        {
        case 0: return "F";
        case 1: return "M";
        default: return "NA";
        }
    };
    std::normal_distribution pheno_dist;
    auto pheno = [&pheno_dist, &mersenne_engine]() {
        return pheno_dist(mersenne_engine);
    };
    mockGenotype geno;
    Reporter reporter("log", 60, true);
    mock_prsice prsice;
    prsice.set_reporter(&reporter);
    geno.set_reporter(&reporter);
    std::vector<double> sample_pheno;
    std::vector<bool> valid_pheno(num_sample, false);
    auto&& sample_with_pheno = prsice.sample_with_phenotypes();
    const std::string delim = " ";
    auto ignore_fid = GENERATE(true, false);
    for (size_t i = 0; i < num_sample; ++i)
    {
        auto cur_pheno = pheno();
        auto valid_sample = valid();
        geno.add_sample(Sample_ID(std::to_string(i), std::to_string(i),
                                  std::to_string(cur_pheno), valid_sample));
        if (valid_sample)
        {
            valid_pheno[i] = true;
            std::string id = std::to_string(i);
            if (!ignore_fid) id.append(delim + std::to_string(i));
            sample_with_pheno[id] = i;
            sample_pheno.push_back(
                misc::convert<double>(std::to_string(cur_pheno)));
        }
    }
    prsice.phenotype_matrix() = Eigen::Map<Eigen::VectorXd>(
        sample_pheno.data(), static_cast<Eigen::Index>(sample_pheno.size()));
    std::vector<std::string> cov_names = {"PC1", "PC2", "Sex", "Age", "Batch"};
    std::vector<size_t> cov_idx = {2, 4, 5, 6, 7};
    std::set<size_t> factor_idx = {5, 7};
    std::string cov_header = "FID IID PC1 Something PC2 Sex Age Batch Centre\n";
    SECTION("Valid input")
    {
        const size_t sample_in_file = 2000;
        std::string cov_file = cov_header;
        std::unordered_map<std::string, size_t> expected_batch, expected_sex;
        size_t batch_count = 0, sex_count = 0;
        std::vector<double> expected_pheno;
        auto&& sample_vec = geno.get_sample_vec();
        for (size_t i = 0; i < sample_in_file; ++i)
        {
            if (i < num_sample)
            {
                if (!sample_vec[i].in_regression)
                {
                    // doesn't matter what we sim
                    cov_file.append(std::to_string(i) + " " + std::to_string(i)
                                    + " " + std::to_string(pheno()) + " "
                                    + std::to_string(pheno()) + " "
                                    + std::to_string(pheno()) + " " + sex()
                                    + " " + std::to_string(pheno()) + " "
                                    + batch() + " " + batch() + "\n");
                }
                else
                {
                    auto pc1 = valid() ? std::to_string(pheno()) : "NA";
                    auto cur_batch = batch();
                    auto cur_sex = sex();
                    if (pc1 != "NA")
                    {
                        expected_pheno.push_back(
                            misc::convert<double>(sample_vec[i].pheno));
                        if (expected_sex.find(cur_sex) == expected_sex.end())
                        { expected_sex[cur_sex] = sex_count++; }
                        if (expected_batch.find(cur_batch)
                            == expected_batch.end())
                        { expected_batch[cur_batch] = batch_count++; }
                    }
                    cov_file.append(std::to_string(i) + " " + std::to_string(i)
                                    + " " + pc1 + " " + std::to_string(pheno())
                                    + " " + std::to_string(pheno()) + " "
                                    + cur_sex + " " + std::to_string(pheno())
                                    + " " + cur_batch + " " + batch() + "\n");
                }
            }
            else
            {
                // just doesn't matter what we simulate
                cov_file.append(std::to_string(i) + " " + std::to_string(i)
                                + " " + std::to_string(pheno()) + " "
                                + std::to_string(pheno()) + " "
                                + std::to_string(pheno()) + " " + sex() + " "
                                + std::to_string(pheno()) + " " + batch() + " "
                                + batch() + "\n");
            }
        }
        std::unique_ptr<std::istream> input_file =
            std::make_unique<std::istringstream>(cov_file);

        auto res = prsice.test_cov_check_and_factor_level_count(
            factor_idx, cov_names, cov_idx, delim, ignore_fid, input_file,
            geno);
        auto sex_factor = res.front();
        auto batch_factor = res.back();
        REQUIRE(sex_factor.size() == expected_sex.size());
        REQUIRE(batch_factor.size() == expected_batch.size());
        for (auto&& b : expected_batch)
        {
            REQUIRE(batch_factor.find(b.first) != batch_factor.end());
            REQUIRE(batch_factor[b.first] == b.second);
        }
        for (auto&& s : expected_sex)
        {
            REQUIRE(sex_factor.find(s.first) != sex_factor.end());
            REQUIRE(sex_factor[s.first] == s.second);
        }
        auto pheno_matrix = prsice.phenotype_matrix();
        REQUIRE(static_cast<size_t>(pheno_matrix.rows())
                == expected_pheno.size());
        for (size_t i = 0; i < expected_pheno.size(); ++i)
        { REQUIRE(pheno_matrix(i, 0) == Approx(expected_pheno[i])); }
    }
    SECTION("Invalid cov file format")
    {
        std::unique_ptr<std::istream> cov_file =
            std::make_unique<std::istringstream>(cov_header
                                                 + "1 1 0.1 NA M 10 B1 c1\n"
                                                   "2 2 0.1 NA M 10\n");

        REQUIRE_THROWS_WITH(prsice.test_cov_check_and_factor_level_count(
                                factor_idx, cov_names, cov_idx, delim,
                                ignore_fid, cov_file, geno),
                            Catch::Contains("Error: Malformed covariate file, "
                                            "should have at least 8 columns"));
    }
    SECTION("Duplicated covariate ID")
    {
        std::string input = cov_header;
        for (size_t i = 0; i < num_sample; ++i)
        {
            input.append(
                std::to_string(i) + " " + std::to_string(i) + " "
                + std::to_string(pheno()) + " " + std::to_string(pheno()) + " "
                + std::to_string(pheno()) + " " + std::to_string(pheno()) + " "
                + std::to_string(pheno()) + " " + std::to_string(pheno())
                + "\n");
            input.append(
                std::to_string(i) + " " + std::to_string(i) + " "
                + std::to_string(pheno()) + " " + std::to_string(pheno()) + " "
                + std::to_string(pheno()) + " " + std::to_string(pheno()) + " "
                + std::to_string(pheno()) + " " + std::to_string(pheno())
                + "\n");
        }
        std::unique_ptr<std::istream> cov_file =
            std::make_unique<std::istringstream>(input);
        REQUIRE_THROWS_WITH(
            prsice.test_cov_check_and_factor_level_count(
                factor_idx, cov_names, cov_idx, delim, ignore_fid, cov_file,
                geno),
            Catch::Contains("duplicated IDs in covariate file"));
    }
    SECTION("No valid samples")
    {
        std::unique_ptr<std::istream> cov_file =
            std::make_unique<std::istringstream>(
                cov_header
                + "1 1 NA 0.1 0.1 M 10 b1 c1\n"
                  "2 2 0.1 0.1 NA M 10 b1 c1\n"
                  "3 3 0.1 0.1 0.1 NA 10 b1 c1\n"
                  "4 4 0.1 0.1 0.1 M NA b1 c1\n"
                  "4 4 0.1 0.1 0.1 M 10 NA c1\n");
        REQUIRE_THROWS_WITH(prsice.test_cov_check_and_factor_level_count(
                                factor_idx, cov_names, cov_idx, delim,
                                ignore_fid, cov_file, geno),
                            Catch::Contains("Error: All samples removed due to "
                                            "missingness in covariate file!"));
    }
}
TEST_CASE("Update valid samples from m_phenotype")
{
    // we need a genotype object with sample initialized (SampleID)
    // we also need to initialize the m_phenotype matrix
    // first generate samples
    const size_t num_sample = 1000;
    std::random_device rnd_device;
    std::mt19937 mersenne_engine {rnd_device()};
    // select ~70% of samples
    std::uniform_int_distribution<size_t> dist {1, 10};
    auto valid = [&dist, &mersenne_engine]() {
        return dist(mersenne_engine) > 7;
    };
    std::normal_distribution pheno_dist;
    auto pheno = [&pheno_dist, &mersenne_engine]() {
        return pheno_dist(mersenne_engine);
    };
    mockGenotype geno;
    Reporter reporter("log", 60, true);
    geno.set_reporter(&reporter);
    std::vector<double> sample_pheno;
    std::vector<bool> valid_after_covariate(num_sample, false);
    size_t num_cov_valid = 0;
    std::vector<double> expected_pheno;
    for (size_t i = 0; i < num_sample; ++i)
    {
        auto cur_pheno = pheno();
        auto valid_sample = valid();
        geno.add_sample(Sample_ID(std::to_string(i), std::to_string(i),
                                  std::to_string(cur_pheno), valid_sample));
        if (valid_sample)
        {
            sample_pheno.push_back(cur_pheno);
            valid_after_covariate[i] = valid();
            if (valid_after_covariate[i])
            {
                expected_pheno.push_back(cur_pheno);
                ++num_cov_valid;
            }
        }
    }

    mock_prsice prsice;
    prsice.phenotype_matrix() = Eigen::Map<Eigen::VectorXd>(
        sample_pheno.data(), static_cast<Eigen::Index>(sample_pheno.size()));
    prsice.test_update_phenotype_matrix(valid_after_covariate, num_cov_valid,
                                        geno);
    auto res = prsice.phenotype_matrix();
    REQUIRE(static_cast<size_t>(res.rows()) == expected_pheno.size());
    for (size_t i = 0; i < expected_pheno.size(); ++i)
    { REQUIRE(res(i, 0) == Approx(expected_pheno[i])); }
}