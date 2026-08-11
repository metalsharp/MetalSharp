import { execFile } from "child_process";

type DmgEjectOptions = {
  shell: false;
  stdio: "ignore";
};

export type DmgEjectExecutor = (
  file: string,
  args: string[],
  options: DmgEjectOptions,
  callback: (error: Error | null) => void,
) => void;

const executeDmgEject: DmgEjectExecutor = (file, args, options, callback) => {
  execFile(file, args, options, (error) => callback(error));
};

export function ejectDmgVolume(volumeName: string, execute: DmgEjectExecutor = executeDmgEject): Promise<void> {
  return new Promise((resolve, reject) => {
    execute("hdiutil", ["detach", `/Volumes/${volumeName}`, "-quiet"], { shell: false, stdio: "ignore" }, (error) => {
      if (error) {
        reject(error);
        return;
      }
      resolve();
    });
  });
}
