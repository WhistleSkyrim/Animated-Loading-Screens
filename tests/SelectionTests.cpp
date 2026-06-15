#include "Media/MediaSelector.h"
#include "TestFramework.h"

ALS_TEST(SelectionModesHandleEmptySequentialAndWeights)
{
    ALS::MediaSelector empty({}, {}, ALS::SelectionMode::Random, false, {});
    ALS::Tests::Expect(!empty.SelectNext().has_value(), "empty random selection should not crash");

    std::vector<ALS::MediaFile> files{
        { "a.mp4", 1, 1, std::nullopt },
        { "b.mp4", 1, 1, std::nullopt }
    };
    ALS::MediaSelector sequential(files, {}, ALS::SelectionMode::Sequential, false, {});
    ALS::Tests::Expect(sequential.SelectNext()->path == "a.mp4", "sequential first file");
    ALS::Tests::Expect(sequential.SelectNext()->path == "b.mp4", "sequential second file");
    ALS::Tests::Expect(sequential.SelectNext()->path == "a.mp4", "sequential wraps");

    std::vector<ALS::MediaFile> weighted{
        { "zero.mp4", 1, 0, std::nullopt },
        { "negative.mp4", 1, -5, std::nullopt },
        { "positive.mp4", 1, 3, std::nullopt }
    };
    ALS::MediaSelector selector(files, weighted, ALS::SelectionMode::WeightedRandom, false, {});
    for (int i = 0; i < 20; ++i) {
        const auto selected = selector.SelectNext();
        ALS::Tests::Expect(selected.has_value(), "weighted selection should return a file");
        ALS::Tests::Expect(selected->path == "positive.mp4", "zero and negative weights should be ignored");
    }

    std::vector<ALS::MediaFile> allDisabled{
        { "zero.mp4", 1, 0, std::nullopt },
        { "negative.mp4", 1, -5, std::nullopt }
    };
    ALS::MediaSelector disabledSelector(files, allDisabled, ALS::SelectionMode::WeightedRandom, false, {});
    ALS::Tests::Expect(!disabledSelector.SelectNext().has_value(), "all zero/negative weights should not fall back to uniform random");
}

ALS_TEST(SelectionRememberLastCommitsOnlyAfterPlaybackStarts)
{
    const auto root = ALS::Tests::MakeTempDir("SelectionRememberLastCommitsOnlyAfterPlaybackStarts");
    const auto statePath = root / "state.txt";

    std::vector<ALS::MediaFile> files{
        { "a.mp4", 1, 1, std::nullopt },
        { "b.mp4", 1, 1, std::nullopt }
    };

    ALS::MediaSelector selector(files, {}, ALS::SelectionMode::Sequential, true, statePath);
    const auto selected = selector.SelectNext();
    ALS::Tests::Expect(selected.has_value(), "selection should return a file");
    ALS::Tests::Expect(!std::filesystem::exists(statePath), "uncommitted selection should not update remembered state");

    selector.CommitSelection(*selected);
    ALS::Tests::Expect(std::filesystem::exists(statePath), "committed selection should update remembered state");

    ALS::MediaSelector resumed(files, {}, ALS::SelectionMode::Sequential, true, statePath);
    const auto resumedSelection = resumed.SelectNext();
    ALS::Tests::Expect(resumedSelection.has_value(), "resumed selector should return a file");
    ALS::Tests::Expect(resumedSelection->path == "b.mp4", "remembered state should resume after committed file");
}
