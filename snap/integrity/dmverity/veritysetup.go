// -*- Mode: Go; indent-tabs-mode: t -*-

/*
 * Copyright (C) 2023 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

package dmverity

import (
	"bufio"
	"bytes"
	"fmt"
	"os/exec"
	"strings"

	"github.com/snapcore/snapd/logger"
	"github.com/snapcore/snapd/osutil"
)

// Info represents the dm-verity related data that:
// 1. are not included in the superblock which is generated by default when running
//    veritysetup.
// 2. need their authenticity verified prior to loading the integrity data into the
//    kernel.
//
// For now, since we are keeping the superblock as it is, this only includes the root hash.
type Info struct {
	RootHash string `json:"root-hash"`
}

func getRootHashFromOutput(output []byte) (rootHash string, err error) {
	scanner := bufio.NewScanner(bytes.NewBuffer(output))
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "Root hash") {
			val := strings.SplitN(line, ":", 2)[1]
			rootHash = strings.TrimSpace(val)
		}
	}

	if err = scanner.Err(); err != nil {
		return "", err
	}

	if len(rootHash) == 0 {
		return "", fmt.Errorf("empty root hash")
	}

	return rootHash, nil
}

// Format runs "veritysetup format" and returns an Info struct which includes the
// root hash. "veritysetup format" calculates the hash verification data for
// dataDevice and stores them in hashDevice. The root hash is retrieved from
// the command's stdout.
func Format(dataDevice string, hashDevice string) (*Info, error) {
	cmd := exec.Command("veritysetup", "format", dataDevice, hashDevice)

	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, osutil.OutputErr(output, err)
	}

	logger.Debugf("cmd: 'veritysetup format %s %s':\n%s", dataDevice, hashDevice, string(output))

	rootHash, err := getRootHashFromOutput(output)
	if err != nil {
		return nil, err
	}

	return &Info{RootHash: rootHash}, nil
}
