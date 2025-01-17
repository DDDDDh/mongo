/**
 *    Copyright (C) 2021-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#pragma once

#include "mongo/db/process_health/health_check_status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"

namespace mongo {

namespace process_health {
/**
 * A deadline future wraps an inputFuture and returns an outputFuture where:
 * - the outputFuture is set to the result of the inputFuture if the inputFuture finishes within
 * timeout.
 * - otherwise the outputFuture returns an error.
 */
template <typename ResultStatus>
class DeadlineFuture {
public:
    DeadlineFuture(std::shared_ptr<executor::TaskExecutor> executor,
                   Future<ResultStatus> inputFuture,
                   Milliseconds timeout)
        : _executor(executor) {
        _outputFuturePromise = std::make_unique<SharedPromise<ResultStatus>>();

        auto swCbHandle = _executor->scheduleWorkAt(
            _executor->now() + timeout, [this](const executor::TaskExecutor::CallbackArgs& cbData) {
                auto lk = stdx::lock_guard(_mutex);

                if (!cbData.status.isOK()) {
                    return;
                }

                if (!get().isReady()) {
                    _outputFuturePromise->setError(
                        Status(ErrorCodes::ExceededTimeLimit, "Deadline future timed out"));
                }
            });

        {
            auto lk = stdx::lock_guard(_mutex);
            if (!swCbHandle.isOK() && !get().isReady()) {
                _outputFuturePromise->setError(swCbHandle.getStatus());
                return;
            }
        }

        _timeoutCbHandle = swCbHandle.getValue();

        _inputFuture = std::move(inputFuture).onCompletion([this](StatusWith<ResultStatus> status) {
            {
                auto lk = stdx::lock_guard(_mutex);
                _executor->cancel(_timeoutCbHandle.get());
                if (!get().isReady()) {
                    _outputFuturePromise->setFrom(status);
                }
                return status;
            }
        });
    }

    ~DeadlineFuture() {
        {
            auto lk = stdx::lock_guard(_mutex);
            _executor->cancel(_timeoutCbHandle.get());
        }
    }

    SharedSemiFuture<ResultStatus> get() const {
        return _outputFuturePromise->getFuture();
    }

private:
    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(4), "DeadlineFuture::_mutex");
    const std::shared_ptr<executor::TaskExecutor> _executor;
    Future<ResultStatus> _inputFuture;
    boost::optional<executor::TaskExecutor::CallbackHandle> _timeoutCbHandle;
    std::unique_ptr<SharedPromise<ResultStatus>> _outputFuturePromise;
};
}  // namespace process_health
}  // namespace mongo
